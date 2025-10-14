#include "wiBacklog.h"
#include "wiMath.h"
#include "wiResourceManager.h"
#include "wiTextureHelper.h"
#include "wiFont.h"
#include "wiSpriteFont.h"
#include "wiImage.h"
#include "wiLua.h"
#include "wiInput.h"
#include "wiPlatform.h"
#include "wiHelper.h"
#include "wiGUI.h"

#include <mutex>
#include <deque>
#include <limits>
#include <iostream>

using namespace wi::graphics;
using namespace std::chrono_literals;

namespace wi::backlog
{
	bool enabled = false;
	bool was_ever_enabled = enabled;
	const float speed = 4000.0f;
	const size_t deletefromline = 500;
	float pos = 5;
	float scroll = 0;
	int historyPos = 0;
	wi::font::Params font_params = []() {
		wi::font::Params params;
		params.color = wi::Color(249, 249, 249, 255);
		return params;
	}();
	wi::Color backgroundColor = wi::Color(29, 29, 29, 255);
	bool refitscroll = false;
	wi::gui::TextInputField inputField;
	wi::gui::GUI GUI;

	bool locked = false;
	bool blockLuaExec = false;
	LogLevel logLevel = LogLevel::Default;
	LogLevel unseen = LogLevel::None;

	std::deque<LogEntry> history;
	std::mutex historyLock;

	struct InternalState
	{
		// These must have common lifetime and destruction order, so keep them together in a struct:
		std::deque<LogEntry> entries;
		std::mutex entriesLock;

		std::string getText()
		{
			std::scoped_lock lck(entriesLock);
			std::string retval;
			_forEachLogEntry_unsafe([&](auto&& entry) {retval += entry.text;});
			return retval;
		}
		inline void _forEachLogEntry_unsafe(std::function<void(const LogEntry&)> cb)
		{
			for (auto& entry : entries)
			{
				cb(entry);
			}
		}
		void writeLogfile()
		{
			static const std::string filename = wi::helper::GetCurrentPath() + "/log.txt";
			std::string text = getText();
			static std::mutex writelocker;
			std::scoped_lock lck(writelocker); // to not write the logfile from multiple threads
			wi::helper::FileWrite(filename, (const uint8_t*)text.c_str(), text.length());
		}

		~InternalState()
		{
			// The object will automatically write out the backlog to the temp folder when it's destroyed
			//	Should happen on application exit
			writeLogfile();
		}
	} internal_state;

	void Toggle()
	{
		enabled = !enabled;
		was_ever_enabled = true;
	}
	void Scroll(float dir)
	{
		scroll += dir;
	}
	void Update(const wi::Canvas& canvas, float dt)
	{
		if (!locked)
		{
			if (wi::input::Press(wi::input::KEYBOARD_BUTTON_HOME))
			{
				Toggle();
			}

			if (isActive())
			{
				if (wi::input::Press(wi::input::KEYBOARD_BUTTON_ESCAPE))
				{
					Toggle();
				}
				if (wi::input::Press(wi::input::KEYBOARD_BUTTON_UP))
				{
					historyPrev();
				}
				if (wi::input::Press(wi::input::KEYBOARD_BUTTON_DOWN))
				{
					historyNext();
				}
				if (wi::input::Down(wi::input::KEYBOARD_BUTTON_PAGEUP))
				{
					Scroll(1000.0f * dt);
				}
				if (wi::input::Down(wi::input::KEYBOARD_BUTTON_PAGEDOWN))
				{
					Scroll(-1000.0f * dt);
				}

				Scroll(wi::input::GetPointer().z * 20);

				static bool created = false;
				if (!created)
				{
					created = true;
					inputField.Create("");
					inputField.SetCancelInputEnabled(false);
					inputField.OnInputAccepted([](wi::gui::EventArgs args) {
						historyPos = 0;
						post(args.sValue);
						LogEntry entry;
						entry.text = args.sValue;
						entry.level = LogLevel::Default;
						{
							std::scoped_lock lock(historyLock);
							history.push_back(entry);
							if (history.size() > deletefromline)
							{
								history.pop_front();
							}
						}
						if (!blockLuaExec)
						{
							wi::lua::RunText(args.sValue);
						}
						else
						{
							post("Lua execution is disabled", LogLevel::Error);
						}
						inputField.SetText("");
					});
					inputField.SetColor(backgroundColor);
				}
				if (inputField.GetState() != wi::gui::ACTIVE)
				{
					inputField.SetAsActive();
				}

			}
			else
			{
				inputField.Deactivate();
			}
		}

		if (enabled)
		{
			pos += speed * dt;
		}
		else
		{
			pos -= speed * dt;
		}
		pos = wi::math::Clamp(pos, -canvas.GetLogicalHeight(), 0);

		inputField.SetSize(XMFLOAT2(canvas.GetLogicalWidth(), 20));
		inputField.SetPos(XMFLOAT2(0, canvas.GetLogicalHeight() - 20 + pos));
		inputField.Update(canvas, dt);
	}
	void Draw(
		const wi::Canvas& canvas,
		CommandList cmd,
		ColorSpace colorspace
	)
	{
		if (!was_ever_enabled)
			return;
		if (pos <= -canvas.GetLogicalHeight())
			return;

		GraphicsDevice* device = GetDevice();
		device->EventBegin("Backlog", cmd);

		wi::image::Params fx = wi::image::Params((float)canvas.GetLogicalWidth(), (float)canvas.GetLogicalHeight());
		fx.pos = XMFLOAT3(0, pos, 0);
		fx.opacity = wi::math::Lerp(0.9f, 0, saturate(-pos / canvas.GetLogicalHeight()));
		fx.color = backgroundColor;
		if (colorspace != ColorSpace::SRGB)
		{
			fx.enableLinearOutputMapping(9);
		}
		wi::image::Draw(nullptr, fx, cmd);

		if (colorspace != ColorSpace::SRGB)
		{
			inputField.sprites[inputField.GetState()].params.enableLinearOutputMapping(9);
			inputField.font.params.enableLinearOutputMapping(9);
		}
		inputField.Render(canvas, cmd);

		Rect rect;
		rect.left = 0;
		rect.right = (int32_t)canvas.GetPhysicalWidth();
		rect.top = 0;
		rect.bottom = int32_t(canvas.LogicalToPhysical(inputField.GetPos().y - 15));
		device->BindScissorRects(1, &rect, cmd);

		DrawOutputText(canvas, cmd, colorspace);

		rect.left = 0;
		rect.right = std::numeric_limits<int>::max();
		rect.top = 0;
		rect.bottom = std::numeric_limits<int>::max();
		device->BindScissorRects(1, &rect, cmd);
		device->EventEnd(cmd);
	}

	void DrawOutputText(
		const wi::Canvas& canvas,
		CommandList cmd,
		ColorSpace colorspace
	)
	{
		wi::font::SetCanvas(canvas); // always set here as it can be called from outside...
		wi::font::Params params = font_params;
		params.cursor = {};
		if (refitscroll)
		{
			float textheight = wi::font::TextHeight(getText(), params);
			float limit = canvas.GetLogicalHeight() - 50;
			if (scroll + textheight > limit)
			{
				scroll = limit - textheight;
			}
			refitscroll = false;
		}
		params.posX = 5;
		params.posY = pos + scroll;
		params.h_wrap = canvas.GetLogicalWidth() - params.posX;
		params.color = font_params.color;
		if (colorspace != ColorSpace::SRGB)
		{
			params.enableLinearOutputMapping(9);
		}

		static std::deque<LogEntry> entriesCopy;

		internal_state.entriesLock.lock();
		// Force copy because drawing text while locking is not safe because an error inside might try to lock again!
		entriesCopy = internal_state.entries;
		internal_state.entriesLock.unlock();

		for (auto& x : entriesCopy)
		{
			switch (x.level)
			{
			case LogLevel::Warning:
				params.color = wi::Color::Warning();
				break;
			case LogLevel::Error:
				params.color = wi::Color::Error();
				break;
			default:
				params.color = font_params.color;
				break;
			}
			params.cursor = wi::font::Draw(x.text, params, cmd);
		}

		unseen = LogLevel::None;
	}

	std::string getText()
	{
		return internal_state.getText();
	}
	// You generally don't want to use this. See notes in header
	void _forEachLogEntry_unsafe(std::function<void(const LogEntry&)> cb)
	{
		internal_state._forEachLogEntry_unsafe(cb);
	}
	void clear()
	{
		std::scoped_lock lck(internal_state.entriesLock);
		internal_state.entries.clear();
		scroll = 0;
	}
	void post(const char* input, LogLevel level)
	{
		if (logLevel > level)
		{
			return;
		}

		std::string str;
		switch (level)
		{
		default:
		case LogLevel::Default:
			str = "";
			break;
		case LogLevel::Warning:
			str = "[Warning] ";
			break;
		case LogLevel::Error:
			str = "[Error] ";
			break;
		}
		str += input;
		str += '\n';
		LogEntry entry;
		entry.text = str;
		entry.level = level;

		internal_state.entriesLock.lock();
		internal_state.entries.push_back(entry);
		if (internal_state.entries.size() > deletefromline)
		{
			internal_state.entries.pop_front();
		}
		internal_state.entriesLock.unlock();

		refitscroll = true;

		switch (level)
		{
		default:
		case LogLevel::Default:
			wi::helper::DebugOut(str, wi::helper::DebugLevel::Normal);
			break;
		case LogLevel::Warning:
			wi::helper::DebugOut(str, wi::helper::DebugLevel::Warning);
			break;
		case LogLevel::Error:
			wi::helper::DebugOut(str, wi::helper::DebugLevel::Error);
			break;
		}

		unseen = std::max(unseen, level);

		if (level >= LogLevel::Error)
		{
			internal_state.writeLogfile();  // will lock mutex
		}
	}

	void post(const std::string& input, LogLevel level)
	{
		post(input.c_str(), level);
	}

	void historyPrev()
	{
		std::scoped_lock lock(historyLock);
		if (!history.empty())
		{
			inputField.SetText(history[history.size() - 1 - historyPos].text);
			inputField.SetAsActive();
			if ((size_t)historyPos < history.size() - 1)
			{
				historyPos++;
			}
		}
	}
	void historyNext()
	{
		std::scoped_lock lock(historyLock);
		if (!history.empty())
		{
			if (historyPos > 0)
			{
				historyPos--;
			}
			inputField.SetText(history[history.size() - 1 - historyPos].text);
			inputField.SetAsActive();
		}
	}

	void setBackgroundColor(wi::Color color)
	{
		backgroundColor = color;
	}
	void setFontSize(int value)
	{
		font_params.size = value;
	}
	void setFontRowspacing(float value)
	{
		font_params.spacingY = value;
	}
	void setFontColor(wi::Color color)
	{
		font_params.color = color;
	}

	bool isActive() { return enabled; }

	void Lock()
	{
		locked = true;
		enabled = false;
	}
	void Unlock()
	{
		locked = false;
	}

	void BlockLuaExecution()
	{
		blockLuaExec = true;
	}
	void UnblockLuaExecution()
	{
		blockLuaExec = false;
	}

	void SetLogLevel(LogLevel newLevel)
	{
		logLevel = newLevel;
	}

	LogLevel GetUnseenLogLevelMax()
	{
		return unseen;
	}
}
