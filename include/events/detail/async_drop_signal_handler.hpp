#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <ranges>
#include <shared_mutex>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <plf_colony.h>

#include <boost/asio.hpp>
#include <boost/asio/experimental/parallel_group.hpp>

#include <events/connection.hpp>
#include <events/signal_handler/callback_policy.hpp>
#include <events/detail/parallel_publish.hpp>


// NOLINTBEGIN(cppcoreguidelines-prefer-member-initializer,hicpp-noexcept-move,performance-noexcept-move-constructor)

namespace events {

template<typename FunctionT, typename PolicyT, typename ExecutorT, typename AllocatorT>
class async_signal_handler;


/**
 * @brief An asynchronous variant of @ref signal_handler. Callback invocations will happen on an ASIO executor.
 *        Callbacks that don't finish before a new signal is published will drop the signal. An ASIO completion token
 *        may optionally be provided when publishing a signal, which will be invoked once all callbacks have completed.
 *        If the signal returns values, these will be passed to the completion token.
 */
template<typename ReturnT, typename... ArgsT, typename ExecutorT, typename AllocatorT>
class [[nodiscard]] async_signal_handler<ReturnT(ArgsT...), callback_policy::drop, ExecutorT, AllocatorT>
    : public std::enable_shared_from_this<async_signal_handler<ReturnT(ArgsT...), callback_policy::drop, ExecutorT, AllocatorT>> {
public:
	using allocator_type = AllocatorT;
	using executor_type = ExecutorT;
	using function_type = ReturnT(ArgsT...);
	using completion_type = std::conditional_t<std::is_same_v<void, ReturnT>, void(), void(std::vector<ReturnT>)>;

private:
	using alloc_traits = std::allocator_traits<AllocatorT>;

	using element_type = std::shared_ptr<std::function<function_type>>;
	using container_allocator_type = typename alloc_traits::template rebind_alloc<element_type>;
	using container_type = plf::colony<element_type, container_allocator_type>;

	using remove_element_type = typename container_type::const_pointer;
	using remove_container_allocator_type = typename alloc_traits::template rebind_alloc<remove_element_type>;
	using remove_container_type = std::vector<remove_element_type, remove_container_allocator_type>;

	struct passkey {
		explicit passkey() = default;
	};

public:
	/// Create a std::shared_ptr<async_signal_handler>
	template<typename... ConstructorArgsT>
	[[nodiscard]]
	static auto create(ConstructorArgsT&&... args)
	    -> std::shared_ptr<async_signal_handler<ReturnT(ArgsT...), callback_policy::drop, ExecutorT, AllocatorT>> {

		return std::make_shared<async_signal_handler<ReturnT(ArgsT...), callback_policy::drop, ExecutorT, AllocatorT>>(
		    passkey{}, std::forward<ConstructorArgsT>(args)...
		);
	}

	/// Create a std::shared_ptr<async_signal_handler> using std::allocate_shared
	template<typename... ConstructorArgsT>
	[[nodiscard]]
	static auto allocate(AllocatorT const& allocator, ConstructorArgsT&&... args)
	    -> std::shared_ptr<async_signal_handler<ReturnT(ArgsT...), callback_policy::drop, ExecutorT, AllocatorT>> {

		return std::allocate_shared<async_signal_handler<ReturnT(ArgsT...), callback_policy::drop, ExecutorT, AllocatorT>>(
			allocator, passkey{}, std::forward<ConstructorArgsT>(args)...
		);
	}

	async_signal_handler([[maybe_unused]] passkey key, ExecutorT const& exec) : executor(exec) {
	}

	template<typename ExecutionContext>
	requires std::convertible_to<ExecutionContext&, boost::asio::execution_context&>
	async_signal_handler([[maybe_unused]] passkey key, ExecutionContext& context) :
		async_signal_handler(key, context.get_executor()) {
	}

	async_signal_handler([[maybe_unused]] passkey key, ExecutorT const& exec, AllocatorT const& alloc) :
		allocator(alloc),
		executor(exec) {
	}

	template<typename ExecutionContext>
	requires std::convertible_to<ExecutionContext&, boost::asio::execution_context&>
	async_signal_handler([[maybe_unused]] passkey key, ExecutionContext& context, AllocatorT const& alloc) :
		async_signal_handler(key, context.get_executor(), alloc) {
	}

	/// The normal copy constructor is disabled so that users are forced to go through the create() method
	async_signal_handler(async_signal_handler const&) = delete;

	/**
	 * @brief Construct a new async_signal_handler that holds the same callbacks as another.
	 *
	 * @details Connection objects from the original signal handler will still only refer to callbacks in that signal
	 *          handler.
	 */
	async_signal_handler([[maybe_unused]] passkey key, async_signal_handler const& other) : executor(other.executor) {
		auto locks = std::scoped_lock{other.callback_mut, other.to_remove_mut};

		if constexpr (alloc_traits::propagate_on_container_copy_assignment::value) {
			allocator = alloc_traits::select_on_container_copy_construction(other.allocator);
		}

		callbacks = container_type{allocator};
		to_remove = remove_container_type{allocator};

		other.remove_invalidated_callbacks();

		callbacks.reserve(other.callbacks.size());

		for (auto const& callback : other.callbacks) {
			if (std::ranges::find(other.to_remove, &callback) == other.to_remove.end()) {
				callbacks.insert(callback);
			}
		}
	}

	/**
	 * @brief Construct a new async_signal_handler that holds the same callbacks as another.
	 *
	 * @details Connection objects from the original signal handler will still only refer to callbacks in that signal
	 *          handler.
	 */
	async_signal_handler([[maybe_unused]] passkey key, async_signal_handler const& other, AllocatorT const& alloc) :
		allocator(alloc),
		executor(other.executor) {

		auto locks = std::scoped_lock{other.callback_mut, other.to_remove_mut};

		other.remove_invalidated_callbacks();
		callbacks.reserve(other.callbacks.size());

		for (auto const& callback : other.callbacks) {
			if (std::ranges::find(other.to_remove, &callback) == other.to_remove.end()) {
				callbacks.insert(callback);
			}
		}
	}

	/**
	 * @brief Construct a new async_signal_handler that will take ownership of another's callbacks
	 *
	 * @details Moving from a handler with running callbacks is undefined behavior. Existing connection objects from
	 *          the original signal handler are invalidated.
	 */
	async_signal_handler([[maybe_unused]] passkey key, async_signal_handler&& other) {
		auto locks = std::scoped_lock{other.callback_mut, other.to_remove_mut};

		assert(
			std::ranges::all_of(std::views::values(other.callbacks), [](auto&& cb) { return static_cast<bool>(cb); })
			&& "Can not move from an async_signal_handler with running callbacks"
		);

		executor = std::move(other.executor);

		if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
			allocator = std::move(other.allocator);
		}

		callbacks = container_type{allocator};
		to_remove = remove_container_type{allocator};

		callbacks = std::move(other.callbacks);
		to_remove = std::move(other.to_remove);
	}

	/**
	 * @brief Construct a new async_signal_handler that will take ownership of another's callbacks
	 *
	 * @details Moving from a handler with running callbacks is undefined behavior. Existing connection objects from
	 *          the original signal handler are invalidated.
	 */
	async_signal_handler([[maybe_unused]] passkey key, async_signal_handler&& other, AllocatorT const& alloc) :
		allocator(alloc) {

		auto locks = std::scoped_lock{other.callback_mut, other.to_remove_mut};

		assert(
			std::ranges::all_of(std::views::values(other.callbacks), [](auto&& cb) { return static_cast<bool>(cb); })
			&& "Can not move from an async_signal_handler with running callbacks"
		);

		executor = std::move(other.executor);

		if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
			allocator = std::move(other.allocator);
		}

		callbacks = std::move(other.callbacks);
		to_remove = std::move(other.to_remove);
	}

	~async_signal_handler() = default;

	/**
	 * @brief Copy the valid callbacks from an async_signal_handler to this one
	 *
	 * @details Assigning to an async_signal_handler that has running callbacks is undefined behavior. Existing
	 *          connection objects from this signal handler are invalidated. Connection objects from the other signal
	 *          handler will still only refer to callbacks in that signal handler.
	 */
	auto operator=(async_signal_handler const& other) -> async_signal_handler& {
		if (&other == this) {
			return *this;
		}

		auto locks = std::scoped_lock{callback_mut, to_remove_mut, other.callback_mut, other.to_remove_mut};

		assert(
			std::ranges::all_of(std::views::values(callbacks), [](auto&& cb) { return static_cast<bool>(cb); })
			&& "Can not copy to an async_signal_handler with running callbacks"
		);

		executor = other.executor;

		if constexpr (alloc_traits::propagate_on_container_copy_assignment::value) {
			allocator = other.allocator;
		}

		other.remove_invalidated_callbacks();
		callbacks.reserve(other.callbacks.size());

		for (auto const& callback : other.callbacks) {
			if (std::ranges::find(other.to_remove, &callback) == other.to_remove.end()) {
				callbacks.insert(callback);
			}
		}

		return *this;
	}

	/**
	 * @brief Move the valid callbacks from an async_signal_handler to this one
	 *
	 * @details Assigning to an async_signal_handler that has running callbacks is undefined behavior, and moving from
	 *          one that has running callbacks is undefined behavior. Existing connection objects from both signal
	 *          handlers are invalidated.
	 */
	auto operator=(async_signal_handler&& other) -> async_signal_handler& {
		if (&other == this) {
			return *this;
		}

		auto locks = std::scoped_lock{callback_mut, to_remove_mut, other.callback_mut, other.to_remove_mut};

		assert(
			std::ranges::all_of(std::views::values(callbacks), [](auto&& cb) { return static_cast<bool>(cb); })
			&& "Can not move to an async_signal_handler with running callbacks"
		);

		assert(
			std::ranges::all_of(std::views::values(other.callbacks), [](auto&& cb) { return static_cast<bool>(cb); })
			&& "Can not move from an async_signal_handler with running callbacks"
		);

		executor = std::move(other.executor);

		if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
			allocator = std::move(other.allocator);
		}

		callbacks = std::move(other.callbacks);
		to_remove = std::move(other.to_remove);

		return *this;
	}

	[[nodiscard]]
	constexpr auto get_allocator() const noexcept -> allocator_type {
		return allocator;
	}

	/// Get the executor associated with this object
	[[nodiscard]]
	auto get_executor() const -> executor_type {
		return executor;
	}

	/// Get the number of callbacks registered with this signal handler
	[[nodiscard]]
	auto size() const -> size_t {
		auto lock = std::scoped_lock{callback_mut};
		return callbacks.size();
	}

	/// Disconnect all callbacks
	auto disconnect_all() -> void {
		auto callback_lock = std::scoped_lock{callback_mut};

		remove_invalidated_callbacks();

		auto remove_lock = std::scoped_lock{to_remove_mut};

		// Remove all callbacks that aren't running, and mark those that are running for deletion.
		for (auto it = callbacks.begin(); it != callbacks.end();) {
			if (*it) {
				it = callbacks.erase(it);
			}
			else {
				to_remove.push_back(&(*it));
				++it;
			}
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
	template<typename FunctionT>
	auto connect(FunctionT&& func) -> connection {
		auto lock = std::unique_lock{callback_mut};
		auto const it = callbacks.insert(std::allocate_shared<std::function<function_type>>(allocator, std::forward<FunctionT>(func)));
		lock.unlock();

		return connection{[weak_self = this->weak_from_this(), ptr = &(*it)] {
			if (auto self = weak_self.lock()) {
				self->disconnect(ptr);
			}
		}};
	}

	/**
	 * @brief Fire the signal synchronously
	 *
	 * @param args The signal arguments
	 */
	auto publish(ArgsT... args) -> void requires std::same_as<void, ReturnT>
	{
		auto pending_callbacks = prune_callbacks_and_get_pending();

		for (auto& [ptr, callback] : pending_callbacks) {
			(*callback)(args...);
			release_callback(ptr, std::move(callback));
		}
	}

	/**
	 * @brief Fire the signal synchronously
	 *
	 * @param args The signal arguments
	 *
	 * @return The callback results
	 */
	auto publish(ArgsT... args) -> std::vector<ReturnT> requires(!std::same_as<void, ReturnT>)
	{
		auto pending_callbacks = prune_callbacks_and_get_pending();

		auto results = std::vector<ReturnT>{};
		results.reserve(pending_callbacks.size());

		for (auto& [ptr, callback] : pending_callbacks) {
			results.emplace_back((*callback)(args...));
			release_callback(ptr, std::move(callback));
		}

		return results;
	}

	/**
	 * @brief Fire the signal asynchronously
	 *
	 * @param args The signal arguments
	 */
	auto async_publish(ArgsT... args) -> void {
		// Store the arguments in a tuple, which will be referenced by each async operation.
		auto args_tuple = std::make_shared<std::tuple<ArgsT...>>(std::forward<ArgsT>(args)...);

		// Delete callbacks marked for removal, and get a list of those that can be executed. No lock on callback_mut
		// needs to be acquired for the rest of this function. This is because the callbacks will be marked as running
		// and thus can not be deleted until released via release_callback().
		auto pending_callbacks = prune_callbacks_and_get_pending();

		for (auto& [ptr, callback] : pending_callbacks) {
			boost::asio::post(executor, [self = this->shared_from_this(), ptr, callback = std::move(callback), args_tuple]() mutable {
				(void)std::apply(*callback, *args_tuple);
				release_callback(ptr, std::move(callback));
			});
		}
	}

	/**
	 * @brief Fire the signal asynchronously and invoke a completion token when finished
	 *
	 * @param args The signal arguments
	 * @param completion A completion token that will be called when the operation completes
	 */
	template<boost::asio::completion_token_for<completion_type> CompletionToken>
	auto async_publish(ArgsT... args, CompletionToken&& completion) {
		auto args_tuple = std::make_shared<std::tuple<ArgsT...>>(std::forward<ArgsT>(args)...);

		auto pending_callbacks = prune_callbacks_and_get_pending();

		// This will post a function which will invoke the callback then re-add the callback to the list of pending
		// callbacks once it has completed. The actual operation is deferred to be later executed as part of a
		// parallel_group.
		auto post_op = [this, &args_tuple](typename container_type::pointer ptr, element_type&& callback) {
			auto execute = [this, ptr, callback = std::move(callback), &args = *args_tuple]() mutable {
				if constexpr (std::same_as<void, ReturnT>) {
					std::apply(*callback, args);
					release_callback(ptr, std::move(callback));
					return boost::asio::deferred_t::values(std::monostate{});  //needs to return a value
				}
				else {
					auto result = std::apply(*callback, args);
					release_callback(ptr, std::move(callback));
					return boost::asio::deferred_t::values(std::move(result));
				}
			};

			return boost::asio::post(executor, boost::asio::deferred(std::move(execute)));
		};

		// Create the deferred callback invocation for each callback
		using post_op_type = decltype(post_op(std::declval<typename container_type::pointer>(), std::declval<element_type&&>()));
		auto operations = std::vector<post_op_type>{};
		operations.reserve(pending_callbacks.size());

		for (auto& [ptr, callback] : pending_callbacks) {
			operations.emplace_back(post_op(ptr, std::move(callback)));
		}

		// Keep this signal handler and the arguments alive until the completion token has been called (when all
		// callbacks have completed).
		auto consigned = boost::asio::consign(
		    std::forward<CompletionToken>(completion), this->shared_from_this(), std::move(args_tuple)
		);

		// Initiate the callbacks as a parallel_group, with a completion that takes either nothing if ReturnT is void,
		// or a vector of the callback results otherwise.
		return detail::parallel_publish<ReturnT>(executor, std::move(operations), std::move(consigned), allocator);
	}

private:
	auto disconnect(typename container_type::const_pointer pointer) -> void {
		auto lock = std::scoped_lock{to_remove_mut};
		to_remove.push_back(pointer);
	}

	auto release_callback(typename container_type::pointer pointer, element_type&& callback) -> void {
		auto lock = std::shared_lock{callback_mut};
		(*callbacks.get_iterator(pointer)) = std::move(callback);
	}

	// Remove invalidated callbacks and get a list of the pending ones
	[[nodiscard]]
	auto prune_callbacks_and_get_pending() -> std::vector<std::pair<typename container_type::pointer, element_type>> {
		// This will be populated with the callbacks that need to be executed
		auto pending_callbacks = std::vector<std::pair<typename container_type::pointer, element_type>>{};

		auto lock = std::scoped_lock{callback_mut};

		// Remove invalidated callbacks that aren't currently executing
		remove_invalidated_callbacks();

		// Gather all pending (not running) callbacks. We don't need to filter out handles in the to_remove set since
		// the function call above will only leave invalidated callbacks that are currently executing, and they can't
		// be released during this block since an exclusive mutex is held the entire time. Thus, they won't be included
		// in this update and will be removed with the next update.
		for (auto& callback : callbacks) {
			if (callback) {
				pending_callbacks.emplace_back(&callback, std::move(callback));
			}
			else {
				//SPDLOG_LOG_TRACE("Callback {:#X} dropped a signal", static_cast<uintptr_t>(&callback));
			}
		}

		return pending_callbacks;
	}

	// Requires an exclusive lock on callback_mut
	auto remove_invalidated_callbacks() -> void {
		auto lock = std::scoped_lock{to_remove_mut};

		// Move all handles whose callbacks aren't currently executing to the end of the vector
		auto const remove_begin = std::partition(to_remove.begin(), to_remove.end(), [&](auto pointer) {
			return static_cast<bool>(*pointer); //if the shared_ptr is null, then the callback is in progress
		});

		// Erase all callbacks that aren't in progress
		for (auto it = remove_begin; it != to_remove.end(); ++it) {
			callbacks.erase(callbacks.get_iterator(*it));
		}

		// Erase all the now-invalid iterators
		auto const count = static_cast<size_t>(std::distance(remove_begin, to_remove.end()));
		to_remove.resize(to_remove.size() - count);
	}


	AllocatorT allocator;

	ExecutorT executor;

	container_type callbacks{allocator};
	std::shared_mutex callback_mut;

	// Tracks which callbacks have been disconnected, so they can be removed when they are no longer executing.
	remove_container_type to_remove{allocator};
	std::mutex to_remove_mut;
};

} //namespace events

// NOLINTEND(cppcoreguidelines-prefer-member-initializer,hicpp-noexcept-move,performance-noexcept-move-constructor)
