#pragma once

#include <concepts>
#include <functional>
#include <memory>
#include <ranges>

#include <plf_colony.h>

#include <events/connection.hpp>


namespace events {

template<typename FunctionT, typename = std::allocator<void>>
class signal_handler;


/**
 * @brief A signal handler allows callbacks to be registered which will be invoked when the signal is published.
 *        Signals can have any function signature, and can also have return values, which will be collected and
 *        returned to the publisher of the signal.
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
	using element_type = std::function<function_type>;
	using container_allocator_type = typename alloc_traits::template rebind_alloc<element_type>;
	using container_type = plf::colony<element_type, container_allocator_type>;

public:
	signal_handler() = default;

	signal_handler(signal_handler const&) = default;

	/**
	 * @brief Construct a new signal_handler that holds the same callbacks as another.
	 *
	 * @details Connection objects from the original signal handler will still only refer to callbacks in that signal
	 *          handler.
	 */
	signal_handler(signal_handler const& other, AllocatorT const& allocator) : callbacks(other.callbacks, allocator) {
	}

	/**
	 * @brief Construct a new signal_handler that will take ownership of another's callbacks
	 *
	 * @details Moving from a handler with running callbacks is undefined behavior. Existing connection objects from
	 *          the original signal handler are invalidated.
	 */
	signal_handler(signal_handler&&) noexcept = default;

	/**
	 * @brief Construct a new signal_handler that will take ownership of another's callbacks
	 *
	 * @details Moving from a handler with running callbacks is undefined behavior. Existing connection objects from
	 *          the original signal handler are invalidated.
	 */
	signal_handler(signal_handler&& other, AllocatorT const& allocator) :
		callbacks(std::move(other.callbacks), allocator) {
	}

	explicit signal_handler(AllocatorT const& allocator) : callbacks(allocator) {
	}

	~signal_handler() = default;

	/**
	 * @brief Copy the callbacks from a signal_handler to this one
	 *
	 * @details Existing connection objects from this signal handler are invalidated. Connection objects from the other
	 *          signal handler will still only refer to callbacks in that signal handler.
	 */
	auto operator=(signal_handler const&) -> signal_handler& = default;

	/**
	 * @brief Move the callbacks from a signal_handler to this one
	 * @details Existing connection objects from both signal handlers are invalidated.
	 */
	auto operator=(signal_handler&&) noexcept -> signal_handler& = default;

	[[nodiscard]]
	constexpr auto get_allocator() const noexcept -> allocator_type {
		return callbacks.get_allocator();
	}

	/// Get the number of callbacks registered with this signal handler
	[[nodiscard]]
	auto size() const noexcept -> size_t {
		return callbacks.size();
	}

	/// Disconnect all callbacks
	auto disconnect_all() -> void {
		callbacks.clear();
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
	auto connect(FunctionT&& callback) -> connection {
		auto const it = callbacks.insert(std::forward<FunctionT>(callback));
		return connection{[this, ptr = &(*it)] { disconnect(ptr); }};
	}

	/**
	 * @brief Fire the signal
	 *
	 * @param args The signal arguments
	 */
	auto publish(ArgsT... args) -> void requires std::same_as<void, ReturnT>
	{
		for (auto& callback : callbacks) {
			callback(args...);
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
		results.reserve(callbacks.size());

		for (auto& callback : callbacks) {
			results.emplace_back(callback(args...));
		}

		return results;
	}

	/**
	 * @brief Fire the signal as a lazily evaluated range
	 * @return A lazily evaluated range, of which each element will be the result of invoking a callback.
	 */
	auto publish_range(ArgsT... args) requires(!std::same_as<void, ReturnT>)
	{
		return callbacks
		    | std::views::transform([... args = std::forward<ArgsT>(args)](auto& callback) mutable -> ReturnT {
			       return callback(args...);
		       });
	}

private:
	auto disconnect(typename container_type::const_pointer callback_pointer) -> void {
		callbacks.erase(callbacks.get_iterator(callback_pointer));
	}

	container_type callbacks;
};

}  //namespace events
