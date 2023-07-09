#include <events/signal_handler/signal_handler.hpp>

#include <iostream>


auto main() -> int {
	auto sigh = events::signal_handler<void(int)>{};

	// Connect a function that matches the signal handler's function signature.
	sigh.connect([](int n) {
		std::cout << "Received signal: " << n << '\n';
	});

	// All connected functions will be invoked with the signal arguments
	sigh.publish(0);

	// The connect() function returns a connection object that can be used to disconnect the listener
	events::connection connect = sigh.connect([](int) {});
	connect.disconnect();

	// scoped_connection will automatically disconnect the listener when it goes out of scope
	events::scoped_connection scoped = sigh.connect([](int) {});

	return 0;
}
