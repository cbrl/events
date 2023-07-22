#pragma once

#include <type_traits>
#include <utility>

#include <boost/asio.hpp>
#include <events/detail/parallel_group.hpp>
#include <events/detail/void_parallel_group.hpp>


namespace events::detail {

/**
 * @brief Publish a set of operations as a parallel_group, discarding the return order and supporting operations that
 *        take a nullary completion handler (i.e. invoke a completion with no parameters).
 *
 * @details Operations with nullary completions will still need to return something (e.g. std::monostate). This
 *          function will discard those results and invoke a completion with no parameters.
 *
 * @tparam ResultsT...  The completion parameters of the operations
 */
template<typename... ResultsT, typename Operations, typename CompletionToken, typename Allocator>
auto parallel_publish(
	auto default_executor,
	Operations&& operations,
	CompletionToken&& completion_token,
	Allocator const& allocator
) {
	// clang-format off
	using completion_type = std::conditional_t<
		std::conjunction_v<std::is_same<void, ResultsT>...>,
		void(),
		void(std::vector<ResultsT>...)
	>;
	// clang-format on

	auto initiation = [&default_executor, &allocator, ops = std::forward<Operations>(operations)](auto completion_handler) mutable {
		if (ops.empty()) {
			auto completion_ex = boost::asio::get_associated_executor(completion_handler, default_executor);

			boost::asio::post(completion_ex, [handler = std::move(completion_handler)]() mutable {
				if constexpr ((std::same_as<void, ResultsT> && ...)) {
					std::move(handler)();
				}
				else {
					std::move(handler)(std::vector<ResultsT>{}...);
				}
			});
		}
		else {
			if constexpr ((std::same_as<void, ResultsT> && ...)) {
				events::detail::make_void_parallel_group(std::move(ops), allocator)
					.async_wait(boost::asio::experimental::wait_for_all{}, std::move(completion_handler));
			}
			else {
				events::detail::make_parallel_group(std::move(ops), allocator)
					.async_wait(boost::asio::experimental::wait_for_all{}, std::move(completion_handler));
			}
		}
	};

	return boost::asio::async_initiate<CompletionToken, completion_type>(std::move(initiation), completion_token);
}

}  //namespace events::detail
