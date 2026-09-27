#pragma once

#include <algorithm>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include <events/connection.hpp>


// NOLINTBEGIN(hicpp-noexcept-move,performance-noexcept-move-constructor)

namespace events {

template<typename FunctionT, typename AllocatorT = std::allocator<void>>
class synchronized_signal_handler;


/**
 * @brief A thread-safe variant of @ref signal_handler
 *
 * @details Uses copy-on-write snapshots for efficient concurrent publishing. Callbacks are stored behind shared
 *          pointers in an immutable snapshot vector. Publishing briefly locks a mutex to copy the snapshot pointer,
 *          then iterates without holding any lock. Mutations (connect/disconnect) create a new snapshot, ensuring
 *          that concurrent publishers continue to iterate over a consistent set of callbacks.
 *
 *          Callbacks are invoked in the order they were connected. A publish checks whether each callback is still
 *          connected right before invoking it, so a callback disconnected during a publish (on any thread) is skipped
 *          if that publish hasn't reached it yet. A newly connected callback is first invoked by the next publish.
 *
 * @warning disconnect() does not wait for invocations that are already running on other threads. Such an invocation
 *          may still be executing (or about to start) when disconnect() returns. A callback that uses an object which
 *          may be destroyed after disconnecting should hold a std::shared_ptr or std::weak_ptr to it.
 */
template<typename ReturnT, typename... ArgsT, typename AllocatorT>
class [[nodiscard]] synchronized_signal_handler<ReturnT(ArgsT...), AllocatorT> {
public:
	using function_type = ReturnT(ArgsT...);
	using allocator_type = AllocatorT;

private:
	using alloc_traits = std::allocator_traits<AllocatorT>;
	using callback_type = std::function<function_type>;

	struct slot {
		template<typename FunctionT>
		explicit slot(FunctionT&& function) : callback(std::forward<FunctionT>(function)) {
		}

		std::uint64_t id = 0;  ///< Only accessed while holding the owning state's mutex
		std::atomic<bool> connected{true};
		callback_type callback;
	};

	using slot_ptr = std::shared_ptr<slot>;
	using snapshot_allocator_type = typename alloc_traits::template rebind_alloc<slot_ptr>;
	using snapshot_type = std::vector<slot_ptr, snapshot_allocator_type>;
	using snapshot_ptr = std::shared_ptr<snapshot_type const>;

	/**
	 * The callbacks. Connections hold weak references to it, so they can safely outlive the signal handler.
	 *
	 * Old snapshots are always released after unlocking the mutex, since that may destroy callbacks, and a callback's
	 * destructor may call back into this state (e.g. by owning a scoped_connection to it).
	 */
	class state final : public detail::slot_owner {
	public:
		explicit state(AllocatorT const& alloc) : allocator(alloc) {
		}

		~state() override = default;

		auto disconnect(std::uint64_t id) noexcept -> void override {
			auto doomed = snapshot_ptr{};
			auto const lock = std::scoped_lock{mutex};

			auto const* const found = find(id);
			if (found == nullptr || !(*found)->connected.exchange(false, std::memory_order_acq_rel)) {
				return;
			}

			--live_count;

			try {
				doomed = std::exchange(snapshot, rebuild());
			}
			catch (...) {
				// Out of memory. The slot stays in the snapshot, but it's flagged as disconnected so it won't be invoked.
				// The next successful change removes it.
			}
		}

		[[nodiscard]] auto connected(std::uint64_t id) const noexcept -> bool override {
			auto const lock = std::scoped_lock{mutex};
			auto const* const found = find(id);
			return (found != nullptr) && (*found)->connected.load(std::memory_order_relaxed);
		}

		auto disconnect_all() noexcept -> void {
			auto doomed = snapshot_ptr{};
			auto const lock = std::scoped_lock{mutex};
			mark_all_disconnected();
			live_count = 0;
			doomed = std::exchange(snapshot, nullptr);
		}

		template<typename FunctionT>
		auto add(FunctionT&& callback) -> std::uint64_t {
			auto const new_slot = std::allocate_shared<slot>(allocator, std::forward<FunctionT>(callback));

			auto doomed = snapshot_ptr{};
			auto const lock = std::scoped_lock{mutex};

			new_slot->id = next_id;
			doomed = std::exchange(snapshot, rebuild(new_slot));

			++live_count;
			return next_id++;
		}

		/// Replace the callbacks with the given slots, which are assigned new ids
		auto assign(snapshot_type replacement) -> void {
			// Allocate before locking, so a failure can't destroy callbacks while holding the lock
			auto new_snapshot = replacement.empty() ? nullptr : make_snapshot(std::move(replacement));

			auto doomed = snapshot_ptr{};
			auto const lock = std::scoped_lock{mutex};

			if (new_snapshot) {
				for (auto const& current : *new_snapshot) {
					current->id = next_id++;
				}
			}

			mark_all_disconnected();
			live_count = new_snapshot ? new_snapshot->size() : 0;
			doomed = std::exchange(snapshot, std::move(new_snapshot));
		}

		/// Remove all callbacks without disconnecting them, so that another state can adopt them
		[[nodiscard]] auto take_all() noexcept -> snapshot_ptr {
			auto const lock = std::scoped_lock{mutex};
			live_count = 0;
			return std::exchange(snapshot, nullptr);
		}

		[[nodiscard]] auto acquire_snapshot() const -> snapshot_ptr {
			auto const lock = std::scoped_lock{mutex};
			return snapshot;
		}

		[[nodiscard]] auto size() const -> size_t {
			auto const lock = std::scoped_lock{mutex};
			return live_count;
		}

		/// Copy the connected callbacks of a snapshot into new slots
		[[nodiscard]] auto copy_slots(snapshot_ptr const& source) const -> snapshot_type {
			auto result = snapshot_type{snapshot_allocator_type{allocator}};
			if (!source) {
				return result;
			}

			result.reserve(source->size());
			for (auto const& current : *source) {
				if (current->connected.load(std::memory_order_acquire)) {
					result.push_back(std::allocate_shared<slot>(allocator, current->callback));
				}
			}
			return result;
		}

		/// Collect the connected slots of a snapshot taken from another state
		[[nodiscard]] auto adopt_slots(snapshot_ptr const& source) const -> snapshot_type {
			auto result = snapshot_type{snapshot_allocator_type{allocator}};
			if (!source) {
				return result;
			}

			result.reserve(source->size());
			for (auto const& current : *source) {
				if (current->connected.load(std::memory_order_acquire)) {
					result.push_back(current);
				}
			}
			return result;
		}

		AllocatorT const allocator;

	private:
		/// Must hold the lock. Snapshots are kept in id order.
		[[nodiscard]] auto find(std::uint64_t id) const noexcept -> slot_ptr const* {
			if (!snapshot) {
				return nullptr;
			}

			auto const it = std::ranges::lower_bound(*snapshot, id, {}, [](slot_ptr const& current) { return current->id; });
			return (it != snapshot->end() && (*it)->id == id) ? std::to_address(it) : nullptr;
		}

		/// Must hold the lock
		auto mark_all_disconnected() noexcept -> void {
			if (snapshot) {
				for (auto const& current : *snapshot) {
					current->connected.store(false, std::memory_order_release);
				}
			}
		}

		/// Must hold the lock. Build a snapshot of the connected slots, plus an optional new one.
		[[nodiscard]] auto rebuild(slot_ptr const& extra = nullptr) const -> snapshot_ptr {
			auto slots = snapshot_type{snapshot_allocator_type{allocator}};
			slots.reserve((snapshot ? snapshot->size() : 0) + (extra ? 1 : 0));

			if (snapshot) {
				for (auto const& current : *snapshot) {
					if (current->connected.load(std::memory_order_relaxed)) {
						slots.push_back(current);
					}
				}
			}
			if (extra) {
				slots.push_back(extra);
			}

			return slots.empty() ? nullptr : make_snapshot(std::move(slots));
		}

		[[nodiscard]] auto make_snapshot(snapshot_type slots) const -> snapshot_ptr {
			return std::allocate_shared<snapshot_type>(allocator, std::move(slots));
		}

		mutable std::mutex mutex;
		snapshot_ptr snapshot;
		std::uint64_t next_id = 1;
		size_t live_count = 0;
	};

public:
	synchronized_signal_handler() : synchronized_signal_handler(AllocatorT{}) {
	}

	explicit synchronized_signal_handler(AllocatorT const& alloc) : impl(std::allocate_shared<state>(alloc, alloc)) {
	}

	/**
	 * @brief Construct a new synchronized_signal_handler that holds the same callbacks as another.
	 *
	 * @details Connection objects from the original signal handler will still only refer to callbacks in that signal
	 *          handler.
	 */
	synchronized_signal_handler(synchronized_signal_handler const& other) :
		synchronized_signal_handler(other, alloc_traits::select_on_container_copy_construction(other.get_allocator())) {
	}

	/**
	 * @brief Construct a new synchronized_signal_handler that holds the same callbacks as another.
	 *
	 * @details Connection objects from the original signal handler will still only refer to callbacks in that signal
	 *          handler.
	 */
	synchronized_signal_handler(synchronized_signal_handler const& other, AllocatorT const& alloc) :
		synchronized_signal_handler(alloc) {
		impl->assign(impl->copy_slots(other.impl->acquire_snapshot()));
	}

	/**
	 * @brief Construct a new synchronized_signal_handler that will take ownership of another's callbacks
	 *
	 * @details Existing connection objects from the original signal handler become no-ops.
	 */
	synchronized_signal_handler(synchronized_signal_handler&& other) :
		synchronized_signal_handler(std::move(other), other.get_allocator()) {
	}

	/**
	 * @brief Construct a new synchronized_signal_handler that will take ownership of another's callbacks
	 *
	 * @details Existing connection objects from the original signal handler become no-ops.
	 */
	synchronized_signal_handler(synchronized_signal_handler&& other, AllocatorT const& alloc) :
		synchronized_signal_handler(alloc) {
		impl->assign(impl->adopt_slots(other.impl->take_all()));
	}

	/// Disconnects all callbacks. Publishes in progress on other threads stop invoking them.
	~synchronized_signal_handler() {
		impl->disconnect_all();
	}

	/**
	 * @brief Copy the connected callbacks from a synchronized_signal_handler to this one
	 *
	 * @details Existing connection objects from this signal handler are disconnected. Connection objects from the other
	 *          signal handler will still only refer to callbacks in that signal handler.
	 */
	auto operator=(synchronized_signal_handler const& other) -> synchronized_signal_handler& {
		if (&other != this) {
			impl->assign(impl->copy_slots(other.impl->acquire_snapshot()));
		}
		return *this;
	}

	/**
	 * @brief Move the connected callbacks from a synchronized_signal_handler to this one
	 *
	 * @details Existing connection objects from this signal handler are disconnected, and existing connection objects
	 *          from the other signal handler become no-ops.
	 */
	auto operator=(synchronized_signal_handler&& other) -> synchronized_signal_handler& {
		if (&other != this) {
			impl->assign(impl->adopt_slots(other.impl->take_all()));
		}
		return *this;
	}

	[[nodiscard]]
	auto get_allocator() const noexcept -> allocator_type {
		return impl->allocator;
	}

	/// Get the number of callbacks connected to this signal handler
	[[nodiscard]]
	auto size() const -> size_t {
		return impl->size();
	}

	/**
	 * @brief Register a callback function that will be invoked when the signal is fired.
	 *
	 * @param callback  A function that is compatible with the signal handler's function signature
	 *
	 * @return A connection handle that can be used to disconnect the function from this signal handler
	 */
	template<std::invocable<ArgsT...> FunctionT>
	requires std::constructible_from<callback_type, FunctionT>
	auto connect(FunctionT&& callback) -> connection {
		auto const id = impl->add(std::forward<FunctionT>(callback));
		return connection{impl, id};
	}

	/// Disconnect all callbacks
	auto disconnect_all() noexcept -> void {
		impl->disconnect_all();
	}

	/**
	 * @brief Fire the signal
	 *
	 * @param args The signal arguments
	 */
	auto publish(ArgsT... args) -> void requires std::same_as<void, ReturnT>
	{
		auto const snap = impl->acquire_snapshot();
		if (!snap) {
			return;
		}

		for (auto const& current : *snap) {
			if (current->connected.load(std::memory_order_acquire)) {
				current->callback(args...);
			}
		}
	}

	/**
	 * @brief Fire the signal
	 *
	 * @param args The signal arguments
	 *
	 * @return The callback results
	 */
	auto publish(ArgsT... args) -> std::vector<ReturnT> requires(!std::same_as<void, ReturnT>)
	{
		auto const snap = impl->acquire_snapshot();

		auto results = std::vector<ReturnT>{};
		if (!snap) {
			return results;
		}

		results.reserve(snap->size());

		for (auto const& current : *snap) {
			if (current->connected.load(std::memory_order_acquire)) {
				results.emplace_back(current->callback(args...));
			}
		}

		return results;
	}

private:
	std::shared_ptr<state> impl;  ///< Never null, and never replaced after construction
};

}  //namespace events

// NOLINTEND(hicpp-noexcept-move,performance-noexcept-move-constructor)
