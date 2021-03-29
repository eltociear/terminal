// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "ControlInteractivity.h"
#include <argb.h>
#include <DefaultSettings.h>
#include <unicode.hpp>
#include <Utf16Parser.hpp>
#include <Utils.h>
#include <WinUser.h>
#include <LibraryResources.h>
#include "../../types/inc/GlyphWidth.hpp"
#include "../../types/inc/Utils.hpp"
#include "../../buffer/out/search.h"

#include "ControlInteractivity.g.cpp"

using namespace ::Microsoft::Console::Types;
using namespace ::Microsoft::Console::VirtualTerminal;
using namespace ::Microsoft::Terminal::Core;
using namespace winrt::Windows::Graphics::Display;
using namespace winrt::Windows::System;
using namespace winrt::Windows::ApplicationModel::DataTransfer;

namespace winrt::Microsoft::Terminal::Control::implementation
{
    ControlInteractivity::ControlInteractivity(IControlSettings settings,
                                               TerminalConnection::ITerminalConnection connection) :
        _touchAnchor{ std::nullopt },
        _lastMouseClickTimestamp{},
        _lastMouseClickPos{},
        _selectionNeedsToBeCopied{ false }
    {
        _core = winrt::make_self<ControlCore>(settings, connection);
    }

    void ControlInteractivity::UpdateSettings()
    {
        _UpdateSystemParameterSettings();
    }

    void ControlInteractivity::Initialize()
    {
        // import value from WinUser (convert from milli-seconds to micro-seconds)
        _multiClickTimer = GetDoubleClickTime() * 1000;
    }

    // Method Description:
    // - Returns the number of clicks that occurred (double and triple click support).
    // Every call to this function registers a click.
    // Arguments:
    // - clickPos: the (x,y) position of a given cursor (i.e.: mouse cursor).
    //    NOTE: origin (0,0) is top-left.
    // - clickTime: the timestamp that the click occurred
    // Return Value:
    // - if the click is in the same position as the last click and within the timeout, the number of clicks within that time window
    // - otherwise, 1
    unsigned int ControlInteractivity::_NumberOfClicks(winrt::Windows::Foundation::Point clickPos,
                                                       Timestamp clickTime)
    {
        // if click occurred at a different location or past the multiClickTimer...
        Timestamp delta;
        THROW_IF_FAILED(UInt64Sub(clickTime, _lastMouseClickTimestamp, &delta));
        if (clickPos != _lastMouseClickPos || delta > _multiClickTimer)
        {
            _multiClickCounter = 1;
        }
        else
        {
            _multiClickCounter++;
        }

        _lastMouseClickTimestamp = clickTime;
        _lastMouseClickPos = clickPos;
        return _multiClickCounter;
    }

    void ControlInteractivity::GainFocus()
    {
        _UpdateSystemParameterSettings();
    }

    // Method Description
    // - Updates internal params based on system parameters
    void ControlInteractivity::_UpdateSystemParameterSettings() noexcept
    {
        if (!SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &_rowsToScroll, 0))
        {
            LOG_LAST_ERROR();
            // If SystemParametersInfoW fails, which it shouldn't, fall back to
            // Windows' default value.
            _rowsToScroll = 3;
        }
    }

    // // Method Description:
    // // - Given a copy-able selection, get the selected text from the buffer and send it to the
    // //     Windows Clipboard (CascadiaWin32:main.cpp).
    // // - CopyOnSelect does NOT clear the selection
    // // Arguments:
    // // - singleLine: collapse all of the text to one line
    // // - formats: which formats to copy (defined by action's CopyFormatting arg). nullptr
    // //             if we should defer which formats are copied to the global setting
    bool ControlInteractivity::CopySelectionToClipboard(bool singleLine,
                                                        const Windows::Foundation::IReference<CopyFormat>& formats)
    {
        if (_core)
        {
            // Note to future self: This should return false if there's no
            // selection to copy. If there's no selection, returning false will
            // indicate that the actions that trigered this should _not_ be
            // marked as handled, so ctrl+c without a selection can still send
            // ^C

            // Mark the current selection as copied
            _selectionNeedsToBeCopied = false;

            return _core->CopySelectionToClipboard(singleLine, formats);
        }

        return false;
    }

    // Method Description:
    // - Initiate a paste operation.
    void ControlInteractivity::PasteTextFromClipboard()
    {
        // attach TermControl::_SendInputToConnection() as the clipboardDataHandler.
        // This is called when the clipboard data is loaded.
        auto clipboardDataHandler = std::bind(&ControlInteractivity::_SendPastedTextToConnection, this, std::placeholders::_1);
        auto pasteArgs = winrt::make_self<PasteFromClipboardEventArgs>(clipboardDataHandler);

        // send paste event up to TermApp
        _PasteFromClipboardHandlers(*this, *pasteArgs);
    }

    // Method Description:
    // - Pre-process text pasted (presumably from the clipboard)
    //   before sending it over the terminal's connection.
    void ControlInteractivity::_SendPastedTextToConnection(const std::wstring& wstr)
    {
        _core->PasteText(winrt::hstring{ wstr });
    }

    // TODO: Don't take a Windows::UI::Input::PointerPoint here. No WInUI here.
    void ControlInteractivity::PointerPressed(const winrt::Windows::UI::Input::PointerPoint point,
                                              const ::Microsoft::Terminal::Core::ControlKeyStates modifiers,
                                              const bool /*focused*/,
                                              const til::point terminalPosition,
                                              const winrt::Windows::Devices::Input::PointerDeviceType type)
    {
        if (type == Windows::Devices::Input::PointerDeviceType::Mouse ||
            type == Windows::Devices::Input::PointerDeviceType::Pen)
        {
            // const auto modifiers = static_cast<uint32_t>(args.KeyModifiers());
            // static_cast to a uint32_t because we can't use the WI_IsFlagSet
            // macro directly with a VirtualKeyModifiers
            const auto altEnabled = modifiers.IsAltPressed(); // WI_IsFlagSet(modifiers, static_cast<uint32_t>(VirtualKeyModifiers::Menu));
            const auto shiftEnabled = modifiers.IsShiftPressed(); // WI_IsFlagSet(modifiers, static_cast<uint32_t>(VirtualKeyModifiers::Shift));
            const auto ctrlEnabled = modifiers.IsCtrlPressed(); // WI_IsFlagSet(modifiers, static_cast<uint32_t>(VirtualKeyModifiers::Control));

            const auto cursorPosition = point.Position();
            // const auto terminalPosition = _GetTerminalPosition(cursorPosition);

            // GH#9396: we prioritize hyper-link over VT mouse events
            //
            // !TODO! Before we'd lock the terminal before getting the hyperlink. Do we still need to?
            auto hyperlink = _core->GetHyperlink(terminalPosition);
            if (point.Properties().IsLeftButtonPressed() &&
                ctrlEnabled && !hyperlink.empty())
            {
                const auto clickCount = _NumberOfClicks(cursorPosition, point.Timestamp());
                // Handle hyper-link only on the first click to prevent multiple activations
                if (clickCount == 1)
                {
                    _HyperlinkHandler(hyperlink);
                }
            }
            else if (_CanSendVTMouseInput(modifiers))
            {
                _TrySendMouseEvent(point, modifiers, terminalPosition);
            }
            else if (point.Properties().IsLeftButtonPressed())
            {
                const auto clickCount = _NumberOfClicks(cursorPosition, point.Timestamp());
                // This formula enables the number of clicks to cycle properly
                // between single-, double-, and triple-click. To increase the
                // number of acceptable click states, simply increment
                // MAX_CLICK_COUNT and add another if-statement
                const unsigned int MAX_CLICK_COUNT = 3;
                const auto multiClickMapper = clickCount > MAX_CLICK_COUNT ? ((clickCount + MAX_CLICK_COUNT - 1) % MAX_CLICK_COUNT) + 1 : clickCount;

                // Capture the position of the first click when no selection is active
                if (multiClickMapper == 1 &&
                    !_core->HasSelection())
                {
                    _singleClickTouchdownPos = cursorPosition;
                    _singleClickTouchdownTerminalPos = terminalPosition;
                    _lastMouseClickPosNoSelection = cursorPosition;
                }
                const bool isOnOriginalPosition = _lastMouseClickPosNoSelection == cursorPosition;

                _core->LeftClickOnTerminal(terminalPosition,
                                           multiClickMapper,
                                           altEnabled,
                                           shiftEnabled,
                                           isOnOriginalPosition,
                                           _selectionNeedsToBeCopied);
            }
            else if (point.Properties().IsRightButtonPressed())
            {
                // CopyOnSelect right click always pastes
                if (_core->CopyOnSelect() || !_core->HasSelection())
                {
                    PasteTextFromClipboard();
                }
                else
                {
                    CopySelectionToClipboard(shiftEnabled, nullptr);
                }
            }
        }
        else if (type == Windows::Devices::Input::PointerDeviceType::Touch)
        {
            const auto contactRect = point.Properties().ContactRect();
            // Set our touch rect, to start a pan.
            _touchAnchor = winrt::Windows::Foundation::Point{ contactRect.X, contactRect.Y };
        }
    }

    // TODO: Doint take a Windows::UI::Input::PointerPoint here
    void ControlInteractivity::PointerMoved(const winrt::Windows::UI::Input::PointerPoint point,
                                            const ::Microsoft::Terminal::Core::ControlKeyStates modifiers,
                                            const bool focused,
                                            const til::point terminalPosition,
                                            const winrt::Windows::Devices::Input::PointerDeviceType type)
    {
        const auto cursorPosition = point.Position();
        if (type == Windows::Devices::Input::PointerDeviceType::Mouse ||
            type == Windows::Devices::Input::PointerDeviceType::Pen)
        {
            // Short-circuit isReadOnly check to avoid warning dialog
            if (focused && !_core->IsInReadOnlyMode() && _CanSendVTMouseInput(modifiers))
            {
                _TrySendMouseEvent(point, modifiers, terminalPosition);
            }
            else if (focused && point.Properties().IsLeftButtonPressed())
            {
                if (_singleClickTouchdownPos)
                {
                    // Figure out if the user's moved a quarter of a cell's smaller axis away from the clickdown point
                    auto& touchdownPoint{ *_singleClickTouchdownPos };
                    auto distance{ std::sqrtf(std::powf(cursorPosition.X - touchdownPoint.X, 2) +
                                              std::powf(cursorPosition.Y - touchdownPoint.Y, 2)) };
                    const til::size fontSize{ _core->GetFont().GetSize() };

                    const auto fontSizeInDips = fontSize.scale(til::math::rounding, 1.0f / _core->RendererScale());
                    if (distance >= (std::min(fontSizeInDips.width(), fontSizeInDips.height()) / 4.f))
                    {
                        // _core->SetSelectionAnchor(_GetTerminalPosition(touchdownPoint));
                        _core->SetSelectionAnchor(terminalPosition);
                        // stop tracking the touchdown point
                        _singleClickTouchdownPos = std::nullopt;
                        _singleClickTouchdownTerminalPos = std::nullopt;
                    }
                }

                _SetEndSelectionPoint(terminalPosition);

                // const double cursorBelowBottomDist = cursorPosition.Y - SwapChainPanel().Margin().Top - SwapChainPanel().ActualHeight();
                // const double cursorAboveTopDist = -1 * cursorPosition.Y + SwapChainPanel().Margin().Top;

                // constexpr double MinAutoScrollDist = 2.0; // Arbitrary value
                // double newAutoScrollVelocity = 0.0;
                // if (cursorBelowBottomDist > MinAutoScrollDist)
                // {
                //     newAutoScrollVelocity = _GetAutoScrollSpeed(cursorBelowBottomDist);
                // }
                // else if (cursorAboveTopDist > MinAutoScrollDist)
                // {
                //     newAutoScrollVelocity = -1.0 * _GetAutoScrollSpeed(cursorAboveTopDist);
                // }

                // if (newAutoScrollVelocity != 0)
                // {
                //     _TryStartAutoScroll(point, newAutoScrollVelocity);
                // }
                // else
                // {
                //     _TryStopAutoScroll(ptr.PointerId());
                // }
            }

            _core->UpdateHoveredCell(terminalPosition);
        }
        else if (focused &&
                 type == Windows::Devices::Input::PointerDeviceType::Touch &&
                 _touchAnchor)
        {
            const auto contactRect = point.Properties().ContactRect();
            winrt::Windows::Foundation::Point newTouchPoint{ contactRect.X, contactRect.Y };
            const auto anchor = _touchAnchor.value();

            // Our actualFont's size is in pixels, convert to DIPs, which the
            // rest of the Points here are in.
            const til::size fontSize{ _core->GetFont().GetSize() };
            const auto fontSizeInDips = fontSize.scale(til::math::rounding, 1.0f / _core->RendererScale());

            // Get the difference between the point we've dragged to and the start of the touch.
            const float dy = newTouchPoint.Y - anchor.Y;

            // Start viewport scroll after we've moved more than a half row of text
            if (std::abs(dy) > (fontSizeInDips.height<float>() / 2.0f))
            {
                // Multiply by -1, because moving the touch point down will
                // create a positive delta, but we want the viewport to move up,
                // so we'll need a negative scroll amount (and the inverse for
                // panning down)
                const float numRows = -1.0f * (dy / fontSizeInDips.height<float>());

                // const auto currentOffset = ::base::ClampedNumeric<double>(ScrollBar().Value());
                const auto currentOffset = ::base::ClampedNumeric<double>(_core->ScrollOffset());
                const auto newValue = numRows + currentOffset;

                // !TODO! - Very worried about using UserScrollViewport as
                // opposed to the ScrollBar().Value() setter. Originally setting
                // the scrollbar value would raise a event handled in
                // _ScrollbarChangeHandler, which would _then_ scroll the core,
                // and start the _updateScrollBar ThrottledFunc to update the
                // scroll position. I'm worried that won't work like this.
                _core->UserScrollViewport(newValue);
                // ScrollBar().Value(newValue);

                // Use this point as our new scroll anchor.
                _touchAnchor = newTouchPoint;
            }
        }
    }

    void ControlInteractivity::PointerReleased(const winrt::Windows::UI::Input::PointerPoint point,
                                               const ::Microsoft::Terminal::Core::ControlKeyStates modifiers,
                                               const bool /*focused*/,
                                               const til::point terminalPosition,
                                               const winrt::Windows::Devices::Input::PointerDeviceType type)
    {
        if (type == Windows::Devices::Input::PointerDeviceType::Mouse ||
            type == Windows::Devices::Input::PointerDeviceType::Pen)
        {
            // Short-circuit isReadOnly check to avoid warning dialog
            if (!_core->IsInReadOnlyMode() && _CanSendVTMouseInput(modifiers))
            {
                _TrySendMouseEvent(point, modifiers, terminalPosition);
                // args.Handled(true);
                return;
            }

            // Only a left click release when copy on select is active should perform a copy.
            // Right clicks and middle clicks should not need to do anything when released.
            if (_core->CopyOnSelect() &&
                point.Properties().PointerUpdateKind() == Windows::UI::Input::PointerUpdateKind::LeftButtonReleased &&
                _selectionNeedsToBeCopied)
            {
                CopySelectionToClipboard(false, nullptr);
            }
        }
        else if (type == Windows::Devices::Input::PointerDeviceType::Touch)
        {
            _touchAnchor = std::nullopt;
        }

        _singleClickTouchdownPos = std::nullopt;
        _singleClickTouchdownTerminalPos = std::nullopt;
    }

    // Method Description:
    // - Send this particular mouse event to the terminal.
    //   See Terminal::SendMouseEvent for more information.
    // Arguments:
    // - point: the PointerPoint object representing a mouse event from our XAML input handler
    bool ControlInteractivity::_TrySendMouseEvent(Windows::UI::Input::PointerPoint const& point,
                                                  const ::Microsoft::Terminal::Core::ControlKeyStates modifiers,
                                                  const til::point terminalPosition)
    {
        const auto props = point.Properties();

        // Get the terminal position relative to the viewport
        // const auto terminalPosition = _GetTerminalPosition(point.Position());

        // Which mouse button changed state (and how)
        unsigned int uiButton{};
        switch (props.PointerUpdateKind())
        {
        case winrt::Windows::UI::Input::PointerUpdateKind::LeftButtonPressed:
            uiButton = WM_LBUTTONDOWN;
            break;
        case winrt::Windows::UI::Input::PointerUpdateKind::LeftButtonReleased:
            uiButton = WM_LBUTTONUP;
            break;
        case winrt::Windows::UI::Input::PointerUpdateKind::MiddleButtonPressed:
            uiButton = WM_MBUTTONDOWN;
            break;
        case winrt::Windows::UI::Input::PointerUpdateKind::MiddleButtonReleased:
            uiButton = WM_MBUTTONUP;
            break;
        case winrt::Windows::UI::Input::PointerUpdateKind::RightButtonPressed:
            uiButton = WM_RBUTTONDOWN;
            break;
        case winrt::Windows::UI::Input::PointerUpdateKind::RightButtonReleased:
            uiButton = WM_RBUTTONUP;
            break;
        default:
            uiButton = WM_MOUSEMOVE;
        }

        // Mouse wheel data
        const short sWheelDelta = ::base::saturated_cast<short>(props.MouseWheelDelta());
        if (sWheelDelta != 0 && !props.IsHorizontalMouseWheel())
        {
            // if we have a mouse wheel delta and it wasn't a horizontal wheel motion
            uiButton = WM_MOUSEWHEEL;
        }

        // const auto modifiers = _GetPressedModifierKeys();
        const TerminalInput::MouseButtonState state{ props.IsLeftButtonPressed(),
                                                     props.IsMiddleButtonPressed(),
                                                     props.IsRightButtonPressed() };
        return _core->SendMouseEvent(terminalPosition, uiButton, modifiers, sWheelDelta, state);
    }

    void ControlInteractivity::_HyperlinkHandler(const std::wstring_view uri)
    {
        // Save things we need to resume later.
        winrt::hstring heldUri{ uri };
        auto hyperlinkArgs = winrt::make_self<OpenHyperlinkEventArgs>(heldUri);
        _OpenHyperlinkHandlers(*this, *hyperlinkArgs);
    }

    bool ControlInteractivity::_CanSendVTMouseInput(const ::Microsoft::Terminal::Core::ControlKeyStates modifiers)
    {
        // If the user is holding down Shift, suppress mouse events
        // TODO GH#4875: disable/customize this functionality
        // const auto modifiers = _GetPressedModifierKeys();
        if (modifiers.IsShiftPressed())
        {
            return false;
        }
        return _core->IsVtMouseModeEnabled();
    }

    // Method Description:
    // - Sets selection's end position to match supplied cursor position, e.g. while mouse dragging.
    // Arguments:
    // - cursorPosition: in pixels, relative to the origin of the control
    void ControlInteractivity::_SetEndSelectionPoint(const til::point terminalPosition)
    {
        _core->SetEndSelectionPoint(terminalPosition);
        _selectionNeedsToBeCopied = true;
    }
}