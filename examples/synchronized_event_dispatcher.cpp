#include <events/dispatcher/synchronized_event_dispatcher.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <iostream>
#include <thread>


struct ContrivedEvent {
	size_t value;
};


auto main() -> int {
	// The synchronized_event_dispatcher is a thread-safe form of the regular event_dispatcher. Multiple threads can
	// connect, enqueue, send, and dispatch at once.
	auto dispatcher = events::synchronized_event_dispatcher{};

	dispatcher.connect<ContrivedEvent>([](auto const& event) {
		std::cout << "Received an event: " << event.value << '\n';
	});

	auto counter = std::atomic_size_t{0};

	// This function will enqueue events and dispatch them every so often
	auto process = [&](std::stop_token stop) {
		while (!stop.stop_requested()) {
			auto const next = counter.fetch_add(1u);

			dispatcher.enqueue<ContrivedEvent>(next);

			if ((next % 100) == 0) {
				dispatcher.dispatch();
			}
		}
	};

	// Run the above function on a few threads
	auto threads = std::array<std::jthread, 3u>{};

	for (auto& thread : threads) {
		thread = std::jthread{process};
	}

	// Sleep for a little while before killing the threads
	std::this_thread::sleep_for(std::chrono::milliseconds{10});

	for (auto& thread : threads) {
		thread.request_stop();
	}
	for (auto& thread : threads) {
		thread.join();
	}

	return 0;
}
