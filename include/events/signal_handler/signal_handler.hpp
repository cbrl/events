#pragma once

#include <concepts>
#include <functional>
#include <ranges>

#include <plf_colony.h>


namespace events {

template<typename...>
class signal_handler;

/**
 * @brief Stores a set of callback functions that will be invoked when the signal is published
 */
template<typename ReturnT, typename... ArgsT>
class [[nodiscard]] signal_handler<ReturnT(ArgsT...)> {
	using container_type = plf::colony<std::function<ReturnT(ArgsT...)>>;

public:
	using function_type = ReturnT(ArgsT...);

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

	/// Disconnect all callbacks
	auto disconnect() -> void {
		callbacks.clear();
	}

private:
	auto disconnect(typename container_type::const_pointer callback_pointer) -> void {
		callbacks.erase(callbacks.get_iterator(callback_pointer));
	}

	container_type callbacks;
};

}  //namespace events
