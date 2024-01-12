#pragma once

#include <atomic>
#include <concepts>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <plf_colony.h>

#include <events/connection.hpp>


// NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer,hicpp-noexcept-move,performance-noexcept-move-constructor)

namespace events {

template<typename FunctionT, typename AllocatorT = std::allocator<void>>
class synchronized_signal_handler;


/**
 * @brief A thread-safe variant of @ref signal_handler
 */
template<typename ReturnT, typename... ArgsT, typename AllocatorT>
class [[nodiscard]] synchronized_signal_handler<ReturnT(ArgsT...), AllocatorT> {
public:
	using function_type = ReturnT(ArgsT...);
	using allocator_type = AllocatorT;

private:
	using alloc_traits = std::allocator_traits<AllocatorT>;

	using element_type = std::function<function_type>;
	using container_allocator_type = typename alloc_traits::template rebind_alloc<element_type>;
	using container_type = plf::colony<element_type, container_allocator_type>;

	using handle_type = uint64_t;
	using handle_container_allocator_type = typename alloc_traits::template rebind_alloc<std::pair<const handle_type, typename container_type::const_pointer>>;
	using handle_container_type = std::unordered_map<handle_type, typename container_type::const_pointer, std::hash<handle_type>, std::equal_to<handle_type>, handle_container_allocator_type>;

	using add_container_allocator_type = typename alloc_traits::template rebind_alloc<std::pair<handle_type, element_type>>;
	using add_container_type = std::vector<std::pair<handle_type, element_type>, add_container_allocator_type>;

	using erase_container_allocator_type = typename alloc_traits::template rebind_alloc<handle_type>;
	using erase_container_type = std::vector<handle_type, erase_container_allocator_type>;

public:
	synchronized_signal_handler() = default;

	explicit synchronized_signal_handler(AllocatorT const& alloc) : callbacks(alloc), to_erase(alloc) {
	}

	/**
	 * @brief Construct a new synchronized_signal_handler that holds the same callbacks as another.
	 *
	 * @details Connection objects from the original signal handler will still only refer to callbacks in that signal
	 *          handler.
	 */
	synchronized_signal_handler(synchronized_signal_handler const& other) {
		auto lock = std::scoped_lock{other.callback_mut, other.handle_mut, other.add_mut, other.erase_mut};

		other.erase_expired_callbacks_impl();
		other.add_pending_callbacks_impl();

		callbacks = other.callbacks;
		next_handle = other.next_handle;
	}

	/**
	 * @brief Construct a new synchronized_signal_handler that holds the same callbacks as another.
	 *
	 * @details Connection objects from the original signal handler will still only refer to callbacks in that signal
	 *          handler.
	 */
	synchronized_signal_handler(synchronized_signal_handler const& other, AllocatorT const& alloc) : to_erase(alloc) {
		auto locks = std::scoped_lock{other.callback_mut, other.handle_mut, other.add_mut, other.erase_mut};

		other.erase_expired_callbacks_impl();
		other.add_pending_callbacks_impl();

		callbacks = container_type{other.callbacks, alloc};
		next_handle = other.next_handle;
	}

	/**
	 * @brief Construct a new synchronized_signal_handler that will take ownership of another's callbacks
	 *
	 * @details Existing connection objects from the original signal handler are invalidated.
	 */
	synchronized_signal_handler(synchronized_signal_handler&& other) {
		auto locks = std::scoped_lock{other.callback_mut, other.handle_mut, other.add_mut, other.erase_mut};

		callbacks = std::move(other.callbacks);
		handles = std::move(other.handles);
		to_add = std::move(other.to_add);
		to_erase = std::move(other.to_erase);
		next_handle = other.next_handle;
	}

	/**
	 * @brief Construct a new synchronized_signal_handler that will take ownership of another's callbacks
	 *
	 * @details Existing connection objects from the original signal handler are invalidated.
	 */
	synchronized_signal_handler(synchronized_signal_handler&& other, AllocatorT const& alloc) {
		auto locks = std::scoped_lock{other.callback_mut, other.handle_mut, other.add_mut, other.erase_mut};

		callbacks = container_type{std::move(other.callbacks), alloc};
		handles = handle_container_type{std::move(other.handles), alloc};
		to_add = add_container_type{std::move(other.to_add), alloc};
		to_erase = erase_container_type{std::move(other.to_erase), alloc};
		next_handle = other.next_handle;
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

		auto locks = std::scoped_lock{
			callback_mut, handle_mut, add_mut, erase_mut,
			other.callback_mut, other.handle_mut, other.add_mut, other.erase_mut
		};

		other.erase_expired_callbacks_impl();
		other.add_pending_callbacks_impl();

		callbacks = other.callbacks;
		next_handle = other.next_handle;

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

		auto locks = std::scoped_lock{
			callback_mut, handle_mut, add_mut, erase_mut,
			other.callback_mut, other.handle_mut, other.add_mut, other.erase_mut
		};

		callbacks = std::move(other.callbacks);
		handles = std::move(other.handles);
		to_add = std::move(other.to_add);
		to_erase = std::move(other.to_erase);
		next_handle = other.next_handle;

		return *this;
	}

	[[nodiscard]]
	constexpr auto get_allocator() const noexcept -> allocator_type {
		return callbacks.get_allocator();
	}

	/// Get the number of callbacks registered with this signal handler
	[[nodiscard]]
	auto size() const noexcept -> size_t {
		auto lock = std::scoped_lock{callback_mut};
		return callbacks.size();
	}

	/**
	 * @brief Register a callback function that will be invoked when the signal is fired. If the callback cannot be
	 *        inserted immediately, then it will be enqueued for later insertion.
	 *
	 * @param callback  A function that is compatible with the signal handler's function signature
	 *
	 * @return A connection handle that can be used to disconnect the function from this signal handler
	 */
	template<std::invocable<ArgsT...> FunctionT>
	auto connect(FunctionT&& callback) -> connection {
		auto const handle = next_handle.fetch_add(1);
		auto callback_ptr = typename container_type::const_pointer{nullptr};

		{
			auto callback_lock = std::unique_lock{callback_mut, std::try_to_lock};

			if (callback_lock) {
				auto const it = callbacks.insert(std::forward<FunctionT>(callback));
				callback_ptr = &(*it);
			}
			else {
				auto add_lock = std::scoped_lock{add_mut};
				to_add.emplace_back(handle, std::forward<FunctionT>(callback));
			}
		}

		{
			auto handle_lock = std::scoped_lock{handle_mut};
			handles[handle] = callback_ptr;
		}

		return connection{[this, handle] { disconnect(handle); }};
	}

	/// Disconnect all callbacks
	auto disconnect_all() -> void {
		auto lock = std::scoped_lock{callback_mut};
		callbacks.clear();
		to_erase.clear();
	}

	/**
	 * @brief Fire the signal
	 *
	 * @param args The signal arguments
	 */
	auto publish(ArgsT... args) -> void requires std::same_as<void, ReturnT>
	{
		erase_expired_callbacks();

		{
			auto lock = std::shared_lock{callback_mut};

			for (auto& callback : callbacks) {
				callback(args...);
			}
		}

		add_pending_callbacks();
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
		erase_expired_callbacks();

		auto results = std::vector<ReturnT>{};
		results.reserve(callbacks.size());

		{
			auto lock = std::shared_lock{callback_mut};

			for (auto& callback : callbacks) {
				results.emplace_back(callback(args...));
			}
		}

		add_pending_callbacks();

		return results;
	}

private:
	auto disconnect(handle_type handle) -> void {
		auto lock = std::scoped_lock{erase_mut};
		to_erase.push_back(handle);
	}

	auto add_pending_callbacks() -> void {
		auto add_lock = std::scoped_lock{add_mut};
		if (to_add.empty()) {
			return;
		}

		auto locks = std::scoped_lock{callback_mut, handle_mut};
		add_pending_callbacks_impl();
	}

	auto add_pending_callbacks_impl() -> void {
		for (auto& [handle, callback] : to_add) {
			auto const it = handles.find(handle);

			// If the handle doesn't exist in the handle map, then this callback was disconnected before it could be
			// inserted into the callback list. In this case, just skip it.
			if (it != handles.end()) {
				continue;
			}

			auto const callback_it = callbacks.insert(std::move(callback));
			it->second = &(*callback_it);
		}

		to_add.clear();
	}

	auto erase_expired_callbacks() -> void {
		auto erase_lock = std::scoped_lock{erase_mut};
		if (to_erase.empty()) {
			return;
		}

		auto locks = std::scoped_lock{callback_mut, handle_mut};
		erase_expired_callbacks_impl();
	}

	// Requires an exclusive lock on callback_mut, handle_mut, and erase_mut
	auto erase_expired_callbacks_impl() -> void {
		for (auto handle : to_erase) {
			auto const it = handles.find(handle);

			// The pointer will be null if the callback is still pending insertion. Nothing special needs to be done
			// in this case other than skipping the actual callback deletion. This case will be detected and the
			// pending callback will be deleted when attempting to add the enqueued callbacks.
			if (it->second) {
				callbacks.erase(callbacks.get_iterator(it->second));
			}

			handles.erase(it);
		}

		to_erase.clear();
	}

	container_type callbacks;
	std::shared_mutex callback_mut;

	handle_container_type handles;
	std::atomic<handle_type> next_handle = 0;
	std::mutex handle_mut;

	add_container_type to_add;
	std::mutex add_mut;

	erase_container_type to_erase;
	std::mutex erase_mut;
};

}  //namespace events

// NOLINTEND(cppcoreguidelines-prefer-member-initializer,hicpp-noexcept-move,performance-noexcept-move-constructor)
