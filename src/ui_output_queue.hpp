#pragma once
#include "types.hpp"

namespace ui_output_queue
{
	struct Message {
		ss_ text;
		ss_ short_text; // For small LCD

		Message(const ss_ &text="", const ss_ &short_text=""):
			text(text), short_text(short_text.empty() ? text : short_text)
		{}
	};

	void clear_messages();
	// Called when a user interaction happens; causes the next push_message()
	// to call clear_messages() before pushing the message.
	// TODO: Call this from hwcontrol
	void unprioritize_queue();
	void push_message(const ss_ &text, const ss_ &short_text="");
	Message get_message();
	void pop_message();
};
