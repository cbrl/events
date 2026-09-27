#pragma once

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <ranges>
#include <tuple>
#include <utility>
#include <vector>

#include <events/connection.hpp>


namespace events {

template<typename FunctionT, typename = std::allocator<void>>
class signal_handler;


/**
 * @brief A signal handler allows callbacks to be registered which will be invoked when the signal is published.
 *        Signals can have any function signature, and can also have return values, which will be collected and
 *        returned to the publisher of the signal.
 *
 * @details Callbacks are invoked in the order they were connected. Callbacks may connect, disconnect, and publish
 *          while the signal is being published:
 *          - A disconnected callback is never invoked again, including later in a publish that is in progress.
 *          - A newly connected callback is first invoked by a publish that starts after the outermost publish in
 *            progress has returned.
 *          - A callback disconnected during a publish is destroyed once the outermost publish returns. Otherwise it's
 *            destroyed before disconnect returns.
 *          - Destroying or reassigning the signal handler from a callback disconnects all of its callbacks, and the
 *            publish in progress stops invoking them.
 *
 * @note Arguments are taken as specified in the function signature. You must explicitly use references in the
 *       signature if you want the publish method to take arguments by reference.
 */
template<typename ReturnT, typename... ArgsT, typename AllocatorT>
class [[nodiscard]] signal_handler<ReturnT(ArgsT...), AllocatorT> {
public:
	using function_type = ReturnT(ArgsT...);
	using allocator_type = AllocatorT;

private:
	using alloc_traits = std::allocator_traits<AllocatorT>;
	using callback_type = std::function<function_type>;

	struct slot {
		std::uint64_t id;
		bool connected;
		callback_type callback;
	};

	using slot_allocator_type = typename alloc_traits::template rebind_alloc<slot>;
	using slot_container_type = std::vector<slot, slot_allocator_type>;

	class state;

	/**
	 * While any deferral_scope is alive, the slot list keeps its size and addresses: disconnected slots are only
	 * flagged and new slots wait in the pending list. The outermost scope applies these changes when it ends.
	 */
	class deferral_scope {
	public:
		explicit deferral_scope(state& owner) noexcept : target(&owner) {
			++owner.depth;
		}

		deferral_scope(deferral_scope const&) = delete;
		deferral_scope(deferral_scope&&) = delete;

		~deferral_scope() {
			// Apply changes while the depth is still 1, so changes made re-entrantly by callback destructors stay deferred
			if (target->depth == 1 && target->has_deferred_work()) {
				target->flush();
			}

			if (--target->depth == 0 && target->self) {
				// Release the state, since the signal handler was destroyed while this scope was active
				[[maybe_unused]] auto const orphan = std::move(target->self);
			}
		}

		auto operator=(deferral_scope const&) -> deferral_scope& = delete;
		auto operator=(deferral_scope&&) -> deferral_scope& = delete;

	private:
		state* target;
	};

	/// The callbacks. Connections hold weak references to it, so they can safely outlive the signal handler.
	class state final : public detail::slot_owner {
	public:
		explicit state(AllocatorT const& alloc) : slots(slot_allocator_type{alloc}), pending(slot_allocator_type{alloc}) {
		}

		/// Copy the connected callbacks of another state. Connections to the other state don't carry over.
		state(state const& other, AllocatorT const& alloc) : state(alloc) {
			slots.reserve(other.live_count);
			for (auto const* const container : {&other.slots, &other.pending}) {
				for (auto const& current : *container) {
					if (current.connected) {
						slots.push_back(current);
					}
				}
			}
			next_id = other.next_id;
			live_count = slots.size();
		}

		~state() override = default;

		auto disconnect(std::uint64_t id) noexcept -> void override {
			auto* const target = find(id);
			if (target == nullptr || !target->connected) {
				return;
			}

			target->connected = false;
			--live_count;
			++dead_count;

			if (depth == 0 && pending.empty()) {
				// Nothing is iterating the slots, so destroy the callback now. Its slot stays behind as a tombstone until
				// the next compaction. The scope defers re-entrant changes made by the callback's destructor.
				[[maybe_unused]] auto const scope = deferral_scope{*this};
				[[maybe_unused]] auto const doomed = std::move(target->callback);
				target->callback = nullptr;
			}
			else {
				// The callback may be running, so destroy it once the outermost publish returns
				needs_destroy = true;
				apply_if_idle();
			}
		}

		[[nodiscard]] auto connected(std::uint64_t id) const noexcept -> bool override {
			auto const* const target = find(id);
			return (target != nullptr) && target->connected;
		}

		auto disconnect_all() noexcept -> void {
			for (auto* const container : {&slots, &pending}) {
				for (auto& current : *container) {
					if (current.connected) {
						current.connected = false;
						++dead_count;
					}
				}
			}
			live_count = 0;
			needs_destroy = true;
			apply_if_idle();
		}

		template<typename FunctionT>
		auto add(FunctionT&& callback) -> std::uint64_t {
			auto new_slot = slot{next_id, true, callback_type(std::forward<FunctionT>(callback))};

			if (depth == 0) {
				merge_pending();  // Only non-empty if an earlier merge ran out of memory
				slots.push_back(std::move(new_slot));
			}
			else {
				pending.push_back(std::move(new_slot));  // A publish is iterating the slots, so they must not move
			}

			++live_count;
			return next_id++;
		}

		[[nodiscard]] auto has_deferred_work() const noexcept -> bool {
			return needs_destroy || !pending.empty() || should_compact();
		}

		/**
		 * Apply deferred changes: append pending slots, destroy the callbacks of disconnected slots, and remove
		 * tombstones once they outnumber the connected slots (which keeps disconnects amortized O(1)). Only called by the
		 * outermost deferral_scope. User code (callback destructors) never runs while the vector is being modified, and
		 * re-entrant changes it makes are deferred and handled by the next iteration of the loop.
		 */
		auto flush() noexcept -> void {
			auto merge_failed = false;
			auto compacted = false;

			while (true) {
				if (!pending.empty() && !merge_failed) {
					try {
						merge_pending();
					}
					catch (...) {
						merge_failed = true;  // Out of memory. The pending slots stay queued until the next flush or connect.
					}
				}

				if (std::exchange(needs_destroy, false)) {
					for (auto& current : slots) {
						if (!current.connected && current.callback) {
							[[maybe_unused]] auto const doomed = std::move(current.callback);
							current.callback = nullptr;
						}
					}
					continue;
				}

				if (should_compact() && !compacted) {
					std::erase_if(slots, [](slot const& current) { return !current.connected; });
					dead_count = count_disconnected();
					compacted = true;
					continue;
				}

				if (merge_failed || pending.empty()) {
					break;
				}
			}
		}

		slot_container_type slots;    ///< In id (connection) order. Doesn't change size while depth > 0.
		slot_container_type pending;  ///< Slots connected while depth > 0. Always newer than everything in slots.

		std::uint64_t next_id = 1;
		std::size_t live_count = 0;  ///< Number of connected slots
		std::size_t dead_count = 0;  ///< Number of disconnected slots that haven't been removed yet
		std::size_t depth = 0;       ///< Number of active deferral_scopes
		bool needs_destroy = false;  ///< Some disconnected slots still hold their callback

		std::shared_ptr<state> self;  ///< Keeps the state alive if its handler is destroyed while depth > 0

	private:
		[[nodiscard]] auto should_compact() const noexcept -> bool {
			return dead_count > live_count;
		}

		[[nodiscard]] auto count_disconnected() const noexcept -> std::size_t {
			auto const is_disconnected = [](slot const& current) { return !current.connected; };
			return static_cast<std::size_t>(std::ranges::count_if(slots, is_disconnected) + std::ranges::count_if(pending, is_disconnected));
		}

		auto apply_if_idle() noexcept -> void {
			if (depth == 0) {
				[[maybe_unused]] auto const scope = deferral_scope{*this};
			}
		}

		auto merge_pending() -> void {
			if (pending.empty()) {
				return;
			}

			// Swap in a fresh pending list first. The moved-from slots may run user code when destroyed, and that code
			// may connect more callbacks.
			auto incoming = std::exchange(pending, slot_container_type{pending.get_allocator()});
			try {
				slots.insert(slots.end(), std::make_move_iterator(incoming.begin()), std::make_move_iterator(incoming.end()));
			}
			catch (...) {
				pending = std::move(incoming);
				throw;
			}
		}

		template<typename ContainerT>
		[[nodiscard]] static auto find_in(ContainerT& container, std::uint64_t id) noexcept -> decltype(container.data()) {
			auto const it = std::ranges::lower_bound(container, id, {}, &slot::id);
			return (it != container.end() && it->id == id) ? std::to_address(it) : nullptr;
		}

		[[nodiscard]] auto find(std::uint64_t id) noexcept -> slot* {
			auto* const found = find_in(slots, id);
			return (found != nullptr) ? found : find_in(pending, id);
		}

		[[nodiscard]] auto find(std::uint64_t id) const noexcept -> slot const* {
			auto const* const found = find_in(slots, id);
			return (found != nullptr) ? found : find_in(pending, id);
		}
	};

public:
	signal_handler() = default;

	explicit signal_handler(AllocatorT const& alloc) noexcept : allocator(alloc) {
	}

	/**
	 * @brief Construct a new signal_handler that holds the same callbacks as another.
	 *
	 * @details Connection objects from the original signal handler will still only refer to callbacks in that signal
	 *          handler.
	 */
	signal_handler(signal_handler const& other) :
		allocator(alloc_traits::select_on_container_copy_construction(other.allocator)),
		impl(other.copy_state(allocator)) {
	}

	/**
	 * @brief Construct a new signal_handler that holds the same callbacks as another.
	 *
	 * @details Connection objects from the original signal handler will still only refer to callbacks in that signal
	 *          handler.
	 */
	signal_handler(signal_handler const& other, AllocatorT const& alloc) : allocator(alloc), impl(other.copy_state(allocator)) {
	}

	/**
	 * @brief Construct a new signal_handler that will take ownership of another's callbacks
	 *
	 * @details Existing connection objects from the other signal handler remain valid, and now refer to this one.
	 */
	signal_handler(signal_handler&& other) noexcept : allocator(other.allocator), impl(std::move(other.impl)) {
	}

	/**
	 * @brief Construct a new signal_handler that will take ownership of another's callbacks
	 *
	 * @details If the allocators are equal, existing connection objects from the other signal handler remain valid and
	 *          now refer to this one. Otherwise the callbacks are copied, and the other handler's connections are
	 *          disconnected.
	 */
	signal_handler(signal_handler&& other, AllocatorT const& alloc) : allocator(alloc) {
		if (alloc_traits::is_always_equal::value || allocator == other.allocator) {
			impl = std::move(other.impl);
		}
		else {
			impl = other.copy_state(allocator);
			other.release_state();
		}
	}

	~signal_handler() {
		release_state();
	}

	/**
	 * @brief Copy the callbacks from a signal_handler to this one
	 *
	 * @details Existing connection objects from this signal handler are disconnected. Connection objects from the other
	 *          signal handler will still only refer to callbacks in that signal handler.
	 */
	auto operator=(signal_handler const& other) -> signal_handler& {
		if (&other == this) {
			return *this;
		}

		if constexpr (alloc_traits::propagate_on_container_copy_assignment::value) {
			auto replacement = other.copy_state(other.allocator);
			release_state();
			allocator = other.allocator;
			impl = std::move(replacement);
		}
		else {
			auto replacement = other.copy_state(allocator);
			release_state();
			impl = std::move(replacement);
		}

		return *this;
	}

	/**
	 * @brief Move the callbacks from a signal_handler to this one
	 *
	 * @details Existing connection objects from this signal handler are disconnected. If the allocator propagates or the
	 *          allocators are equal, connection objects from the other signal handler remain valid and now refer to this
	 *          one. Otherwise the callbacks are copied, and the other handler's connections are disconnected.
	 */
	auto operator=(signal_handler&& other) noexcept(
	    alloc_traits::propagate_on_container_move_assignment::value || alloc_traits::is_always_equal::value
	) -> signal_handler& {
		if (&other == this) {
			return *this;
		}

		if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
			release_state();
			allocator = other.allocator;
			impl = std::move(other.impl);
		}
		else {
			if (alloc_traits::is_always_equal::value || allocator == other.allocator) {
				release_state();
				impl = std::move(other.impl);
			}
			else {
				auto replacement = other.copy_state(allocator);
				release_state();
				impl = std::move(replacement);
				other.release_state();
			}
		}

		return *this;
	}

	[[nodiscard]]
	constexpr auto get_allocator() const noexcept -> allocator_type {
		return allocator;
	}

	/// Get the number of callbacks connected to this signal handler
	[[nodiscard]]
	auto size() const noexcept -> size_t {
		return impl ? impl->live_count : 0;
	}

	/// Disconnect all callbacks
	auto disconnect_all() noexcept -> void {
		if (impl) {
			impl->disconnect_all();
		}
	}

	/**
	 * @brief Register a callback function that will be invoked when the signal is fired
	 *
	 * @tparam FunctionT
	 *
	 * @param callback  A function that is compatible with the signal handler's function signature
	 *
	 * @return A connection handle that can be used to disconnect the function from this signal handler
	 */
	template<std::invocable<ArgsT...> FunctionT>
	requires std::constructible_from<callback_type, FunctionT>
	auto connect(FunctionT&& callback) -> connection {
		auto& owner = get_or_create_state();
		auto owner_ref = std::weak_ptr<detail::slot_owner>{impl};
		auto const id = owner.add(std::forward<FunctionT>(callback));
		return connection{std::move(owner_ref), id};
	}

	/**
	 * @brief Fire the signal
	 *
	 * @param args The signal arguments
	 */
	auto publish(ArgsT... args) -> void requires std::same_as<void, ReturnT>
	{
		if (!impl) {
			return;
		}

		// Only `owner` may be used from here on, since a callback may destroy or reassign this signal handler
		auto& owner = *impl;
		[[maybe_unused]] auto const scope = deferral_scope{owner};

		for (auto i = size_t{0}, count = owner.slots.size(); i < count; ++i) {
			if (auto& current = owner.slots[i]; current.connected) {
				current.callback(args...);
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
		auto results = std::vector<ReturnT>{};
		if (!impl) {
			return results;
		}

		// Only `owner` may be used from here on, since a callback may destroy or reassign this signal handler
		auto& owner = *impl;
		[[maybe_unused]] auto const scope = deferral_scope{owner};

		results.reserve(owner.live_count);

		for (auto i = size_t{0}, count = owner.slots.size(); i < count; ++i) {
			if (auto& current = owner.slots[i]; current.connected) {
				results.emplace_back(current.callback(args...));
			}
		}

		return results;
	}

	/**
	 * @brief Fire the signal as a lazily evaluated range
	 *
	 * @details The range counts as a publish in progress until it (and every copy of it) is destroyed, so callbacks
	 *          connected in the meantime aren't part of it. Avoid keeping it longer than necessary.
	 *
	 * @return A lazily evaluated range, of which each element will be the result of invoking a callback.
	 */
	auto publish_range(ArgsT... args) requires(!std::same_as<void, ReturnT>)
	{
		auto* const owner = &get_or_create_state();
		auto const scope = std::allocate_shared<deferral_scope>(allocator, *owner);
		auto const count = owner->slots.size();

		// A tuple keeps reference parameters as references, where a by-value init-capture would decay them
		return std::views::iota(size_t{0}, count)
		    | std::views::filter([owner, scope](size_t i) { return owner->slots[i].connected; })
		    | std::views::transform([owner, scope, arguments = std::tuple<ArgsT...>{std::forward<ArgsT>(args)...}](size_t i) -> ReturnT {
			       return std::apply(owner->slots[i].callback, arguments);
		       });
	}

private:
	auto get_or_create_state() -> state& {
		if (!impl) {
			impl = std::allocate_shared<state>(allocator, allocator);
		}
		return *impl;
	}

	[[nodiscard]] auto copy_state(AllocatorT const& alloc) const -> std::shared_ptr<state> {
		if (!impl) {
			return nullptr;
		}
		return std::allocate_shared<state>(alloc, *impl, alloc);
	}

	/// Detach from the current state. All of its callbacks are disconnected, so its connections become no-ops.
	auto release_state() noexcept -> void {
		if (!impl) {
			return;
		}

		auto& owner = *impl;
		owner.disconnect_all();

		if (owner.depth > 0) {
			owner.self = std::move(impl);  // A publish is in progress. Keep the state alive until it finishes.
		}

		impl.reset();
	}

	[[no_unique_address]] AllocatorT allocator{};
	std::shared_ptr<state> impl;
};

}  //namespace events
