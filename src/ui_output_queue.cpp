#include "ui_output_queue.hpp"
#include "types.hpp"
#include "print.hpp"

namespace ui_output_queue {

static sv_<Message> message_queue;
bool message_queue_is_unimportant = false;

void clear_messages()
{
	message_queue.clear();
}

void unprioritize_queue()
{
	message_queue_is_unimportant = true;
}

void push_message(const ss_ &text, const ss_ &short_text)
{
	if(message_queue_is_unimportant)
		clear_messages();
	message_queue_is_unimportant = false;
	message_queue.push_back(Message(text, short_text));
}

Message get_message()
{
	if(message_queue.empty())
		return Message();
	return message_queue[0];
}

void pop_message()
{
	if(message_queue.empty()){
		fprintf_(stderr, "ERROR: pop_message(): Queue is already empty\n");
		return;
	}
	message_queue.erase(message_queue.begin(), message_queue.begin()+1);
}


} // namespace ui_output_queue
