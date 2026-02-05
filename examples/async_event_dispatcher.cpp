#include <events/dispatcher/async_event_dispatcher.hpp>
#include <boost/asio.hpp>

#include <iostream>


auto main() -> int {
	auto context = boost::asio::io_context{};
	auto dispatcher = events::async_event_dispatcher{context};

	dispatcher.connect<int>([](int n) {
		std::cout << "Event value: " << n << '\n' << std::flush;
	});

	for (int i = 0; i < 10; ++i) {
		dispatcher.enqueue<int>(i);
	}

	// The async_dispatch() function will dispatch all events to the io_context. Note that the regular dispatch()
	// function will dispatch on the caller thread instead.
	// Because async_dispatch() requires posting functors carrying additional state to the execution context, this
	// usage pattern can cause much higher memory usage in situations where very large numbers of events are expected.
	dispatcher.async_dispatch();
	context.run();
	context.restart();

	for (int i = 10; i < 20; ++i) {
		dispatcher.enqueue<int>(i);
	}

	// async_dispatch() can also take an ASIO completion token. Note that this method requires gathering all invocation
	// operations for all dispatchers before launching them, causing an even larger upfront memory allocation than the
	// overload of async_dispatch() that takes no parameters.
	dispatcher.async_dispatch([] {
		std::cout << "Dispatch completed\n" << std::flush;
	});
	context.run();
	context.restart();

	return 0;
}
