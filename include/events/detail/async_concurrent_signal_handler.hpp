#pragma once

#include <algorithm>
#include <cassert>
#include <concepts>
#include <functional>
#include <memory>
#include <mutex>
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
 * @brief A signal handler that invokes callbacks asynchronously. Callbacks that don't finish before a new signal is
 *        published will still be invoked.
 */
template<typename ReturnT, typename... ArgsT, typename ExecutorT, typename AllocatorT>
class [[nodiscard]] async_signal_handler<ReturnT(ArgsT...), callback_policy::concurrent, ExecutorT, AllocatorT>
    : public std::enable_shared_from_this<async_signal_handler<ReturnT(ArgsT...), callback_policy::concurrent, ExecutorT, AllocatorT>> {
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

	struct passkey {
		explicit passkey() = default;
	};

public:
	/// Create a std::shared_ptr<async_signal_handler>
	template<typename... ConstructorArgsT>
	[[nodiscard]]
	static auto create(ConstructorArgsT&&... args)
	    -> std::shared_ptr<async_signal_handler<ReturnT(ArgsT...), callback_policy::concurrent, ExecutorT, AllocatorT>> {

		return std::make_shared<async_signal_handler<ReturnT(ArgsT...), callback_policy::concurrent, ExecutorT, AllocatorT>>(
		    passkey{}, std::forward<ConstructorArgsT>(args)...
		);
	}

	/// Create a std::shared_ptr<async_signal_handler> using std::allocate_shared
	template<typename... ConstructorArgsT>
	[[nodiscard]]
	static auto allocate(AllocatorT const& allocator, ConstructorArgsT&&... args)
	    -> std::shared_ptr<async_signal_handler<ReturnT(ArgsT...), callback_policy::concurrent, ExecutorT, AllocatorT>> {

		return std::allocate_shared<async_signal_handler<ReturnT(ArgsT...), callback_policy::concurrent, ExecutorT, AllocatorT>>(
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

	/// Construct a new async_signal_handler that holds the same callbacks as another
	async_signal_handler([[maybe_unused]] passkey key, async_signal_handler const& other) {
		auto lock = std::shared_lock{other.callback_mut};

		if constexpr (alloc_traits::propagate_on_container_copy_assignment::value) {
			allocator = other.allocator;
		}

		callbacks = other.callbacks;
	}

	/// Construct a new async_signal_handler that holds the same callbacks as another
	async_signal_handler([[maybe_unused]] passkey key, async_signal_handler const& other, AllocatorT const& alloc) :
		allocator(alloc) {
		auto lock = std::shared_lock{other.callback_mut};
		callbacks = other.callbacks;
	}

	/**
	 * @brief Construct a new async_signal_handler that will take ownership of another's callbacks
	 * @details Moving from a handler with running callbacks is undefined behavior.
	 */
	async_signal_handler([[maybe_unused]] passkey key, async_signal_handler&& other) {
		auto lock = std::scoped_lock{other.callback_mut};

		if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
			allocator = std::move(other.allocator);
		}

		callbacks = std::move(other.callbacks);
	}

	/**
	 * @brief Construct a new async_signal_handler that will take ownership of another's callbacks
	 * @details Moving from a handler with running callbacks is undefined behavior.
	 */
	async_signal_handler([[maybe_unused]] passkey key, async_signal_handler&& other, AllocatorT const& alloc) :
		allocator(alloc) {
		auto lock = std::scoped_lock{other.callbacks_mut};
		callbacks = container_type{std::move(other.callbacks), allocator};
	}

	~async_signal_handler() = default;

	/**
	 * @brief Copy the valid callbacks from an async_signal_handler to this one
	 * @details Assigning to an async_signal_handler that has running callbacks is undefined behavior
	 */
	auto operator=(async_signal_handler const& other) -> async_signal_handler& {
		if (&other == this) {
			return *this;
		}

		auto lock = std::shared_lock{callback_mut, other.callbacks_mut};

		if constexpr (alloc_traits::propagate_on_container_copy_assignment::value) {
			allocator = other.allocator;
		}

		callbacks = other.callbacks;

		return *this;
	}

	/**
	 * @brief Move the valid callbacks from an async_signal_handler to this one
	 *
	 * @details Assigning to an async_signal_handler that has running callbacks is undefined behavior, and moving from
	 *          one that has running callbacks is undefined behavior.
	 */
	auto operator=(async_signal_handler&& other) -> async_signal_handler& {
		if (&other == this) {
			return *this;
		}

		auto lock = std::scoped_lock{callback_mut, other.callbacks_mut};

		if constexpr (alloc_traits::propagate_on_container_move_assignment::value) {
			allocator = std::move(other.allocator);
		}

		callbacks = std::move(other.callbacks);

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
		auto const it = callbacks.insert(std::make_shared<std::function<function_type>>(std::forward<FunctionT>(func)));
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
		auto lock = std::shared_lock{callback_mut};

		for (auto& callback_ptr : callbacks) {
			(*callback_ptr)(args...);
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
		auto lock = std::shared_lock{callback_mut};

		auto results = std::vector<ReturnT>{};
		results.reserve(callbacks.size());

		for (auto& callback_ptr : callbacks) {
			results.emplace_back((*callback_ptr)(args...));
		}

		return results;
	}

	/**
	 * @brief Fire the signal asynchronously
	 *
	 * @param args The signal arguments
	 */
	auto async_publish(ArgsT... args) -> void {
		auto args_tuple = std::make_shared<std::tuple<ArgsT...>>(std::forward<ArgsT>(args)...);

		auto lock = std::shared_lock{callback_mut};

		for (auto& callback_ptr : callbacks) {
			boost::asio::post(executor, [self = this->shared_from_this(), callback_ptr, args_tuple]() mutable {
				(void)std::apply(*callback_ptr, args_tuple);
			});
		}
	}

	/**
	 * @brief Fire the signal asynchronously and invoke a completion token when finished
	 *
	 * @param args The signal arguments
	 * @param completion A completion token that will be called when the operation completes
	 */
	template<typename CompletionToken>
	auto async_publish(ArgsT... args, CompletionToken&& completion) {
		// Store the arguments in a tuple, which will be referenced by each async operation.
		auto args_tuple = std::make_shared<std::tuple<ArgsT...>>(std::forward<ArgsT>(args)...);

		// This will post a function which will invoke the callback then re-add itself to the list of pending callbacks
		// once it has completed. The actual operation is deferred to be later executed as part of a parallel_group.
		auto post_op = [this, &args_tuple](typename container_type::reference callback_ptr) {
			auto execute = [callback_ptr, &args = *args_tuple]() mutable {
				if constexpr (std::same_as<void, ReturnT>) {
					std::apply(*callback_ptr, args);
					return boost::asio::deferred_t::values(std::monostate{});  //needs to return a value
				}
				else {
					auto result = std::apply(*callback_ptr, args);
					return boost::asio::deferred_t::values(std::move(result));
				}
			};

			return boost::asio::post(executor, boost::asio::deferred(std::move(execute)));
		};

		using post_op_type = decltype(post_op(std::declval<typename container_type::reference>()));
		auto operations = std::vector<post_op_type>{};
		operations.reserve(callbacks.size());

		auto lock = std::shared_lock{callback_mut};

		// Create a deferred callback invocation for each callback
		for (auto& ptr : callbacks) {
			operations.emplace_back(post_op(ptr));
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
		auto lock = std::scoped_lock{callback_mut};
		callbacks.erase(callbacks.get_iterator(pointer));
	}

	AllocatorT allocator;

	ExecutorT executor;

	container_type callbacks{allocator};
	std::shared_mutex callback_mut;
};

}  //namespace events

// NOLINTEND(cppcoreguidelines-prefer-member-initializer,hicpp-noexcept-move,performance-noexcept-move-constructor)
