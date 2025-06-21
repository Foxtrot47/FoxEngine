#pragma once
#include <queue>
#include <bitset>

class Keyboard
{
	friend class Window;
public:
	class Event
	{
	public:
		enum class Type
		{
			Pressed,
			Released,
			Invalid
		};
	private:
		Type type;
		unsigned char code;
	public:
		Event() : type(Type::Invalid), code(0u)
		{
		}
		Event(Type type, unsigned char code) : type(type), code(code)
		{
		}
		bool IsPressed() const
		{
			return type == Type::Pressed;
		}
		bool IsReleased() const
		{
			return type == Type::Released;
		}
		bool IsValid() const
		{
			return type != Type::Invalid;
		}
		unsigned char GetCode() const
		{
			return code;
		}
	};

public:
	Keyboard() = default;
	Keyboard(const Keyboard&) = delete;
	Keyboard& operator=(const Keyboard&) = delete;

	// Key events
	bool KeyIsPressed(unsigned char keycode) const;
	Event ReadyKey();
	bool KeyIsEmpty() const;
	void FlushKey();

private:
	void OnKeyPressed(unsigned char keycode);
	void OnKeyReleased(unsigned char keycode);
	void ClearState();
	template<typename T>
	static void TrimBuffer(std::queue<T>& buffer);
private:
	static constexpr unsigned int nKeys = 256u;
	static constexpr unsigned int bufferSize = 16u;
	std::bitset<nKeys> keyStates;
	std::queue<Event> keyBuffer;
	std::queue<char> charBuffer;

};

