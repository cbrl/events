#include <events/dispatcher/event_dispatcher.hpp>

#include <iostream>


struct ContrivedEvent {
	int value;
};


auto main() -> int {
	auto dispatcher = events::event_dispatcher{};

	// Listeners can be connected much like a signal_handler. Howver, this method requires a template parameter that
	// indicates the type of event this function is subscribing to.
	events::connection connection = dispatcher.connect<ContrivedEvent>([](auto const& event) {
		std::cout << "Received an event: " << event.value << '\n';
	});

	// Events can be enqueued for later dispatch
	dispatcher.enqueue(ContrivedEvent{0});
	dispatcher.enqueue<ContrivedEvent>(1);

	// The send() method will immediately invoke all listeners instead of enqueuing the event
	dispatcher.send(ContrivedEvent{2});

	// dispatch() will send all enqueued events
	dispatcher.dispatch();

	return 0;
}
