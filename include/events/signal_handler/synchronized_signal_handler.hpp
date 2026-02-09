#pragma once

#include <concepts>
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
 */
template<typename ReturnT, typename... ArgsT, typename AllocatorT>
class [[nodiscard]] synchronized_signal_handler<ReturnT(ArgsT...), AllocatorT> {
public:
	using function_type = ReturnT(ArgsT...);
	using allocator_type = AllocatorT;

private:
	using alloc_traits = std::allocator_traits<AllocatorT>;

	using callback_type = std::function<function_type>;
	using callback_ptr = std::shared_ptr<callback_type>;

	using snapshot_element_alloc_type = typename alloc_traits::template rebind_alloc<callback_ptr>;
	using snapshot_type = std::vector<callback_ptr, snapshot_element_alloc_type>;
	using snapshot_ptr = std::shared_ptr<snapshot_type const>;

public:
	synchronized_signal_handler() = default;

	explicit synchronized_signal_handler(AllocatorT const& alloc) : allocator(alloc) {
	}

	/**
	 * @brief Construct a new synchronized_signal_handler that holds the same callbacks as another.
	 *
	 * @details Connection objects from the original signal handler will still only refer to callbacks in that signal
	 *          handler. The new handler shares the existing immutable callback snapshot.
	 */
	synchronized_signal_handler(synchronized_signal_handler const& other) {
		auto lock = std::scoped_lock{other.mutex};
		snapshot = other.snapshot;
		allocator = alloc_traits::select_on_container_copy_construction(other.allocator);
	}

	/**
	 * @brief Construct a new synchronized_signal_handler that holds the same callbacks as another.
	 *
	 * @details Connection objects from the original signal handler will still only refer to callbacks in that signal
	 *          handler.
	 */
	synchronized_signal_handler(synchronized_signal_handler const& other, AllocatorT const& alloc) : allocator(alloc) {
		auto lock = std::scoped_lock{other.mutex};
		snapshot = other.snapshot;
	}

	/**
	 * @brief Construct a new synchronized_signal_handler that will take ownership of another's callbacks
	 *
	 * @details Existing connection objects from the original signal handler are invalidated.
	 */
	synchronized_signal_handler(synchronized_signal_handler&& other) {
		auto lock = std::scoped_lock{other.mutex};
		snapshot = std::move(other.snapshot);
		allocator = std::move(other.allocator);
	}

	/**
	 * @brief Construct a new synchronized_signal_handler that will take ownership of another's callbacks
	 *
	 * @details Existing connection objects from the original signal handler are invalidated.
	 */
	synchronized_signal_handler(synchronized_signal_handler&& other, AllocatorT const& alloc) : allocator(alloc) {
		auto lock = std::scoped_lock{other.mutex};
		snapshot = std::move(other.snapshot);
	}

	~synchronized_signal_handler() = default;

	/**
	 * @brief Copy the valid callbacks from a synchronized_signal_handler to this one
	 *
	 * @details Existing connection objects from this signal handler are invalidated. Connection objects from the other
	 *          signal handler will still only refer to callbacks in that signal handler.
	 */
	auto operator=(synchronized_signal_handler const& other) -> synchronized_signal_handler& {
		if (&other == this) {
			return *this;
		}

		auto locks = std::scoped_lock{mutex, other.mutex};
		snapshot = other.snapshot;

		return *this;
	}

	/**
	 * @brief Move the valid callbacks from a synchronized_signal_handler to this one
	 * @details Existing connection objects from both signal handlers are invalidated.
	 */
	auto operator=(synchronized_signal_handler&& other) -> synchronized_signal_handler& {
		if (&other == this) {
			return *this;
		}

		auto locks = std::scoped_lock{mutex, other.mutex};
		snapshot = std::move(other.snapshot);

		return *this;
	}

	[[nodiscard]]
	constexpr auto get_allocator() const noexcept -> allocator_type {
		return allocator;
	}

	/// Get the number of callbacks registered with this signal handler
	[[nodiscard]]
	auto size() const noexcept -> size_t {
		auto lock = std::scoped_lock{mutex};
		return snapshot ? snapshot->size() : 0;
	}

	/**
	 * @brief Register a callback function that will be invoked when the signal is fired.
	 *
	 * @param callback  A function that is compatible with the signal handler's function signature
	 *
	 * @return A connection handle that can be used to disconnect the function from this signal handler
	 */
	template<std::invocable<ArgsT...> FunctionT>
	auto connect(FunctionT&& callback) -> connection {
		auto cb = std::allocate_shared<callback_type>(allocator, std::forward<FunctionT>(callback));
		auto* raw = cb.get();

		{
			auto lock = std::scoped_lock{mutex};
			auto new_snap = copy_snapshot();
			new_snap.push_back(std::move(cb));
			snapshot = make_snapshot(std::move(new_snap));
		}

		return connection{[this, raw] { disconnect(raw); }};
	}

	/// Disconnect all callbacks
	auto disconnect_all() -> void {
		auto lock = std::scoped_lock{mutex};
		snapshot.reset();
	}

	/**
	 * @brief Fire the signal
	 *
	 * @param args The signal arguments
	 */
	auto publish(ArgsT... args) -> void requires std::same_as<void, ReturnT>
	{
		auto snap = acquire_snapshot();
		if (!snap) {
			return;
		}

		for (auto const& cb : *snap) {
			(*cb)(args...);
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
		auto snap = acquire_snapshot();

		auto results = std::vector<ReturnT>{};
		if (!snap) {
			return results;
		}

		results.reserve(snap->size());

		for (auto const& cb : *snap) {
			results.emplace_back((*cb)(args...));
		}

		return results;
	}

private:
	auto disconnect(callback_type* raw) -> void {
		auto lock = std::scoped_lock{mutex};
		if (!snapshot) {
			return;
		}

		auto new_snap = snapshot_type{snapshot_element_alloc_type{allocator}};
		new_snap.reserve(snapshot->size());

		bool found = false;

		for (auto const& cb : *snapshot) {
			if (!found && cb.get() == raw) {
				found = true;
				continue;
			}
			new_snap.push_back(cb);
		}

		if (!found) {
			return;
		}

		snapshot = new_snap.empty() ? nullptr : make_snapshot(std::move(new_snap));
	}

	[[nodiscard]]
	auto acquire_snapshot() const -> snapshot_ptr {
		auto lock = std::scoped_lock{mutex};
		return snapshot;
	}

	[[nodiscard]]
	auto copy_snapshot() const -> snapshot_type {
		if (snapshot) {
			return snapshot_type{*snapshot, snapshot_element_alloc_type{allocator}};
		}
		return snapshot_type{snapshot_element_alloc_type{allocator}};
	}

	[[nodiscard]]
	auto make_snapshot(snapshot_type snap) const -> snapshot_ptr {
		return std::allocate_shared<snapshot_type>(allocator, std::move(snap));
	}

	snapshot_ptr snapshot;
	mutable std::mutex mutex;
	[[no_unique_address]] AllocatorT allocator{};
};

}  //namespace events

// NOLINTEND(hicpp-noexcept-move,performance-noexcept-move-constructor)
