#include "Keeyboard.h"

bool Keyboard::KeyIsPressed(unsigned char keycode) const
{
	return keyStates[keycode];
}

Keyboard::Event Keyboard::ReadyKey()
{
	if (keyBuffer.size() > 0u)
	{
		Keyboard::Event e = keyBuffer.front();
		keyBuffer.pop();
		return e;
	}
	else {
		return Keyboard::Event();
	}
}

bool Keyboard::KeyIsEmpty() const
{
	return keyBuffer.empty();
}

void Keyboard::FlushKey()
{
	keyBuffer = std::queue<Event>();
}

void Keyboard::OnKeyPressed(unsigned char keycode)
{
	keyStates[keycode] = true;
	keyBuffer.push(Event(Event::Type::Pressed, keycode));
	TrimBuffer(keyBuffer);
}

void Keyboard::OnKeyReleased(unsigned char keycode)
{
	keyStates[keycode] = false;
	keyBuffer.push(Event(Event::Type::Released, keycode));
	TrimBuffer(keyBuffer);
}

void Keyboard::ClearState()
{
	keyStates.reset();
}

template<typename T>
void Keyboard::TrimBuffer(std::queue<T>& buffer)
{
	while (buffer.size() > bufferSize)
	{
		buffer.pop();
	}
}
