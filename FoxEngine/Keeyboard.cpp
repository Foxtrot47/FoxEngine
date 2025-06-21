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

char Keyboard::ReadChar()
{
	if (charBuffer.size() > 0u)
	{
		unsigned char charCode = charBuffer.front();
		charBuffer.pop();
		return charCode;
	}
	else {
		return 0;
	}
}

bool Keyboard::CharIsEmpty() const
{
	return charBuffer.empty();
}

void Keyboard::FlushChar()
{
	keyBuffer = std::queue<Event>();
}

void Keyboard::Flush()
{
	FlushKey();
	FlushChar();
}

void Keyboard::EnableAutorepeat()
{
	autorepeatEnabled = true;
}

void Keyboard::DisableAutorepeat()
{
	autorepeatEnabled = false;
}

bool Keyboard::AutorepeatIsEnabled() const
{
	return autorepeatEnabled;
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

void Keyboard::OnChar(char character)
{
	charBuffer.push(character);
	TrimBuffer(charBuffer);
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
