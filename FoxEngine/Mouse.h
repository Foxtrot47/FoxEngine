#pragma once
#include <queue>

class Mouse
{
	friend class Window;
public:
	class Event
	{
	public:
		enum class Type
		{
			LPress,
			LRelease,
			RPress,
			RRelease,
			WheelUp,
			WheelDown,
			Move,
			Enter,
			Leave,
			Invalid
		};
	private:
		Type type;
		bool leftButtonIsPressed;
		bool rightButtonIsPressed;
		int x;
		int y;
	public:
		Event() :
			type(Type::Invalid),
			leftButtonIsPressed(false),
			rightButtonIsPressed(false),
			x(0),
			y(0)
		{
		}
		Event(Type type, const Mouse& parent)
			:
			type(type),
			leftButtonIsPressed(parent.leftIsPressed),
			rightButtonIsPressed(parent.rightIsPressed),
			x(parent.x),
			y(parent.y)
		{
		}
		bool IsValid() const
		{
			return type != Type::Invalid;
		}
		Type GetType() const
		{
			return type;
		}
		std::pair<int, int> GetPos() const
		{
			return{ x,y };
		}
		int GetPosX() const
		{
			return x;
		}
		int GetPosY() const
		{
			return y;
		}
		bool LeftButtonIsPressed() const
		{
			return leftButtonIsPressed;
		}
		bool RightButtonIsPressed() const
		{
			return rightButtonIsPressed;
		}
	};
	struct RawDelta
	{
		int x,y;
	};
public:
	Mouse() : x(0), y(0) {}
	Mouse(const Mouse&) = delete;
	Mouse& operator=(const Mouse&) = delete;
	std::pair<int, int> GetPos() const;
	int GetPosX() const;
	int GetPosY() const;
	RawDelta GetRawDelta();
	bool IsInWindow() const;
	bool LeftIsPressed() const;
	bool RightIsPressed() const;
	Mouse::Event Read();
	bool IsEmpty() const
	{
		return buffer.empty();
	}
	void Flush();
private:
	void OnMouseMove(int x, int y);
	void OnMouseRawDelta(int x, int y);
	void OnMouseLeave();
	void OnMouseEnter();
	void OnLeftButtonPressed(int x, int y);
	void OnLeftButtonReleased(int x, int y);
	void OnRightButtonPressed(int x, int y);
	void OnRightButtonReleased(int x, int y);
	void OnWheelUp(int x, int y);
	void OnWheelDown(int x, int y);
	void TrimBuffer();
	void OnWheelDelta(int x, int y, int delta);
private:
	static constexpr unsigned int bufferSize = 16u;
	int x;
	int y;
	bool leftIsPressed = false;
	bool rightIsPressed = false;
	bool isInWindow = false;
	int wheelDeltaCarry = 0;
	std::queue<Event> buffer;
	std::queue<RawDelta> rawDeltaBuffer;
};

