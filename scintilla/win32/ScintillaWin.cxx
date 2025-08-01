// Scintilla source code edit control
/** @file ScintillaWin.cxx
 ** Windows specific subclass of ScintillaBase.
 **/
// Copyright 1998-2003 by Neil Hodgson <neilh@scintilla.org>
// The License.txt file describes the conditions under which this software may be distributed.

#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cassert>
#include <cstring>
#include <cstdio>
#include <cmath>
#include <climits>

#include <stdexcept>
#include <new>
#include <string>
#include <string_view>
#include <vector>
#include <array>
#include <map>
#include <optional>
#include <algorithm>
#include <memory>
//#include <mutex>

// WIN32_LEAN_AND_MEAN is defined to avoid including commdlg.h
// (which defined FindText) to fix GCC LTO ODR violation warning.

#include <windows.h>
#include <commctrl.h>
#include <richedit.h>
#include <windowsx.h>
#include <zmouse.h>
#include <ole2.h>
#include <shlwapi.h>
#include <shlobj.h>
#include <shellapi.h>

#define DebugCopyAsRichTextFormat		0
#define DebugDragAndDropDataFormat		0
#define MaxDragAndDropDataFormatCount	6
/*
CF_VSSTGPROJECTITEMS, CF_VSREFPROJECTITEMS
https://docs.microsoft.com/en-us/visualstudio/extensibility/ux-guidelines/application-patterns-for-visual-studio
*/
#define EnableDrop_VisualStudioProjectItem		1
/*
Chromium Web Custom MIME Data Format
Used by VSCode, Atom etc.
*/
#define Enable_ChromiumWebCustomMIMEDataFormat	0

#include "ParallelSupport.h"
#include "ScintillaTypes.h"
#include "ScintillaMessages.h"
#include "ScintillaStructures.h"
#include "ILoader.h"
#include "ILexer.h"

#include "Debugging.h"
#include "Geometry.h"
#include "Platform.h"
#include "VectorISA.h"

//#include "CharacterCategory.h"
#include "Position.h"
#include "UniqueString.h"
#include "SplitVector.h"
#include "Partitioning.h"
#include "RunStyles.h"
#include "ContractionState.h"
#include "CellBuffer.h"
#include "CallTip.h"
#include "KeyMap.h"
#include "Indicator.h"
#include "LineMarker.h"
#include "Style.h"
#include "ViewStyle.h"
#include "CharClassify.h"
#include "Decoration.h"
#include "CaseFolder.h"
#include "Document.h"
#include "CaseConvert.h"
#include "UniConversion.h"
#include "Selection.h"
#include "PositionCache.h"
#include "EditModel.h"
#include "MarginView.h"
#include "EditView.h"
#include "Editor.h"
//#include "ElapsedPeriod.h"

#include "AutoComplete.h"
#include "ScintillaBase.h"

#include "WinTypes.h"
#include "PlatWin.h"
#include "SurfaceD2D.h"
#include "HanjaDic.h"
#include "LaTeXInput.h"

#define APPM_DROPFILES				(WM_APP + 7)
#ifndef WM_DPICHANGED
#define WM_DPICHANGED				0x02E0
#endif
#ifndef WM_DPICHANGED_AFTERPARENT
#define WM_DPICHANGED_AFTERPARENT	0x02E3
#endif
#ifndef WM_MOUSEHWHEEL
#define WM_MOUSEHWHEEL				0x020E
#endif
#ifndef SPI_GETWHEELSCROLLCHARS
#define SPI_GETWHEELSCROLLCHARS		0x006C
#endif

using namespace Scintilla;
using namespace Scintilla::Internal;

namespace {

// Two idle messages SC_WIN_IDLE and SC_WORK_IDLE.

// SC_WIN_IDLE is low priority so should occur after the next WM_PAINT
// It is for lengthy actions like wrapping and background styling
constexpr UINT SC_WIN_IDLE = 5001;
// SC_WORK_IDLE is high priority and should occur before the next WM_PAINT
// It is for shorter actions like restyling the text just inserted
// and delivering SCN_UPDATEUI
constexpr UINT SC_WORK_IDLE = 5002;

#if _WIN32_WINNT < _WIN32_WINNT_WIN8
using SetCoalescableTimerSig = UINT_PTR (WINAPI *)(HWND hwnd, UINT_PTR nIDEvent,
	UINT uElapse, TIMERPROC lpTimerFunc, ULONG uToleranceDelay);
#endif

constexpr const WCHAR *callClassName = L"CallTip";

inline void SetWindowID(HWND hWnd, int identifier) noexcept {
	::SetWindowLongPtr(hWnd, GWLP_ID, identifier);
}

constexpr POINT POINTFromLParam(sptr_t lParam) noexcept {
	return { GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) };
};

constexpr Point PointFromLParam(LPARAM lParam) noexcept {
	return Point::FromInts(GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam));
}

inline bool KeyboardIsKeyDown(int key) noexcept {
	return (::GetKeyState(key) & 0x8000) != 0;
}

// Bit 24 is the extended keyboard flag and the numeric keypad is non-extended
constexpr sptr_t extendedKeyboard = 1 << 24;

constexpr bool KeyboardIsNumericKeypadFunction(Scintilla::uptr_t wParam, Scintilla::sptr_t lParam) noexcept {
	if ((lParam & extendedKeyboard) != 0) {
		// Not from the numeric keypad
		return false;
	}

	switch (wParam) {
	case VK_INSERT:	// 0
	case VK_END:	// 1
	case VK_DOWN:	// 2
	case VK_NEXT:	// 3
	case VK_LEFT:	// 4
	case VK_CLEAR:	// 5
	case VK_RIGHT:	// 6
	case VK_HOME:	// 7
	case VK_UP:		// 8
	case VK_PRIOR:	// 9
		return true;
	default:
		return false;
	}
}

inline CLIPFORMAT RegisterClipboardType(LPCWSTR lpszFormat) noexcept {
	// Registered clipboard format values are 0xC000 through 0xFFFF.
	// RegisterClipboardFormat() returns 32-bit unsigned and CLIPFORMAT is 16-bit
	// unsigned so choose the low 16-bits with &.
	return static_cast<CLIPFORMAT>(::RegisterClipboardFormat(lpszFormat));
}

inline RECT GetClientRect(HWND hwnd) noexcept {
	RECT rect{};
	::GetClientRect(hwnd, &rect);
	return rect;
}

inline D2D1_SIZE_U GetSizeUFromRect(const RECT &rc) noexcept {
	const SIZE size = SizeOfRect(rc);
	return D2D1::SizeU(size.cx, size.cy);
}

}

namespace Scintilla::Internal {
// Forward declaration for COM interface subobjects
class ScintillaWin;
}

namespace {

/**
 */
class FormatEnumerator final : public IEnumFORMATETC {
	ULONG ref;
	ULONG pos;
	std::vector<CLIPFORMAT> formats;

public:
	FormatEnumerator(ULONG pos_, const CLIPFORMAT formats_[], size_t formatsLen_);

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, PVOID *ppv) noexcept override;
	STDMETHODIMP_(ULONG)AddRef() noexcept override;
	STDMETHODIMP_(ULONG)Release() noexcept override;

	// IEnumFORMATETC
	STDMETHODIMP Next(ULONG celt, FORMATETC *rgelt, ULONG *pceltFetched) noexcept override;
	STDMETHODIMP Skip(ULONG celt) noexcept override;
	STDMETHODIMP Reset() noexcept override;
	STDMETHODIMP Clone(IEnumFORMATETC **ppenum) override;
};

/**
 */
class DropSource final : public IDropSource {
public:
	ScintillaWin *sci = nullptr;

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, PVOID *ppv) noexcept override;
	STDMETHODIMP_(ULONG)AddRef() noexcept override;
	STDMETHODIMP_(ULONG)Release() noexcept override;

	// IDropSource
	STDMETHODIMP QueryContinueDrag(BOOL fEsc, DWORD grfKeyState) noexcept override;
	STDMETHODIMP GiveFeedback(DWORD) noexcept override;
};

/**
 */
class DataObject final : public IDataObject {
public:
	ScintillaWin *sci = nullptr;

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, PVOID *ppv) noexcept override;
	STDMETHODIMP_(ULONG)AddRef() noexcept override;
	STDMETHODIMP_(ULONG)Release() noexcept override;

	// IDataObject
	STDMETHODIMP GetData(FORMATETC *pFEIn, STGMEDIUM *pSTM) override;
	STDMETHODIMP GetDataHere(FORMATETC *, STGMEDIUM *) noexcept override;
	STDMETHODIMP QueryGetData(FORMATETC *pFE) noexcept override;
	STDMETHODIMP GetCanonicalFormatEtc(FORMATETC *, FORMATETC *pFEOut) noexcept override;
	STDMETHODIMP SetData(FORMATETC *, STGMEDIUM *, BOOL) noexcept override;
	STDMETHODIMP EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC **ppEnum) override;
	STDMETHODIMP DAdvise(FORMATETC *, DWORD, IAdviseSink *, PDWORD) noexcept override;
	STDMETHODIMP DUnadvise(DWORD) noexcept override;
	STDMETHODIMP EnumDAdvise(IEnumSTATDATA **) noexcept override;
};

/**
 */
class DropTarget final : public IDropTarget {
public:
	ScintillaWin *sci = nullptr;

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, PVOID *ppv) noexcept override;
	STDMETHODIMP_(ULONG)AddRef() noexcept override;
	STDMETHODIMP_(ULONG)Release() noexcept override;

	// IDropTarget
	STDMETHODIMP DragEnter(LPDATAOBJECT pIDataSource, DWORD grfKeyState, POINTL pt, PDWORD pdwEffect) override;
	STDMETHODIMP DragOver(DWORD grfKeyState, POINTL pt, PDWORD pdwEffect) override;
	STDMETHODIMP DragLeave() override;
	STDMETHODIMP Drop(LPDATAOBJECT pIDataSource, DWORD grfKeyState, POINTL pt, PDWORD pdwEffect) override;
};

// InputLanguage() and SetCandidateWindowPos() are based on Chromium's IMM32Manager and InputMethodWinImm32.
// https://github.com/chromium/chromium/blob/main/ui/base/ime/win/imm32_manager.cc
// See License.txt or https://github.com/chromium/chromium/blob/main/LICENSE for license details.

// See Chromium's IMM32Manager::SetInputLanguage()
LANGID InputLanguage() noexcept {
	// Retrieve the current input language from the system's keyboard layout.
	// Using GetKeyboardLayoutName instead of GetKeyboardLayout, because
	// the language from GetKeyboardLayout is the language under where the
	// keyboard layout is installed. And the language from GetKeyboardLayoutName
	// indicates the language of the keyboard layout itself.
	// See crbug.com/344834.
	LANGID inputLang;
	WCHAR keyboard_layout[KL_NAMELENGTH];
	if (::GetKeyboardLayoutNameW(keyboard_layout)) {
		inputLang = static_cast<LANGID>(wcstol(&keyboard_layout[KL_NAMELENGTH >> 1], nullptr, 16));
	} else {
		/// TODO: Fallback to en-US?
		HKL inputLocale = ::GetKeyboardLayout(0);
		inputLang = LOWORD(inputLocale);
	}
	//Platform::DebugPrintf("InputLanguage(): %04X\n", inputLang);
	return inputLang;
}

class IMContext {
	HWND hwnd;
	HIMC hIMC;
public:
	explicit IMContext(HWND hwnd_) noexcept :
		hwnd(hwnd_), hIMC(::ImmGetContext(hwnd_)) {}

	// Deleted so IMContext objects can not be copied.
	IMContext(const IMContext &) = delete;
	IMContext(IMContext &&) = delete;
	IMContext &operator=(const IMContext &) = delete;
	IMContext &operator=(IMContext &&) = delete;

	~IMContext() {
		if (hIMC) {
			::ImmReleaseContext(hwnd, hIMC);
		}
	}

	[[nodiscard]] explicit operator bool() const noexcept {
		return hIMC != HIMC{};
	}

	[[nodiscard]] LONG GetImeCaretPos() const noexcept {
		return ::ImmGetCompositionStringW(hIMC, GCS_CURSORPOS, nullptr, 0);
	}

	[[nodiscard]] std::vector<BYTE> GetImeAttributes() const {
		const LONG attrLen = ::ImmGetCompositionStringW(hIMC, GCS_COMPATTR, nullptr, 0);
		std::vector<BYTE> attr(attrLen, 0);
		::ImmGetCompositionStringW(hIMC, GCS_COMPATTR, attr.data(), static_cast<DWORD>(attr.size()));
		return attr;
	}

	[[nodiscard]] bool HasCompositionString(DWORD dwIndex) const noexcept {
		return ::ImmGetCompositionStringW(hIMC, dwIndex, nullptr, 0) > 0;
	}

	[[nodiscard]] LONG GetCompositionStringLength(DWORD dwIndex) const noexcept {
		const LONG byteLen = ::ImmGetCompositionStringW(hIMC, dwIndex, nullptr, 0);
		return byteLen / sizeof(wchar_t);
	}

	[[nodiscard]] std::wstring GetCompositionString(DWORD dwIndex) const {
		const LONG byteLen = ::ImmGetCompositionStringW(hIMC, dwIndex, nullptr, 0);
		std::wstring wcs(byteLen / sizeof(wchar_t), 0);
		::ImmGetCompositionStringW(hIMC, dwIndex, wcs.data(), byteLen);
		return wcs;
	}

	BOOL SetCompositionString(DWORD dwIndex, LPVOID lpComp, DWORD dwCompLen) const noexcept {
		return ::ImmSetCompositionStringW(hIMC, dwIndex, lpComp, dwCompLen, nullptr, 0);
	}

	// See Chromium's IMM32Manager::MoveImeWindow()
	void SetCandidateWindowPos(Point pos, LANGID inputLang, int lineHeight, int sysCaretWidth, int sysCaretHeight) const noexcept {
		const int x = static_cast<int>(pos.x);
		int y = static_cast<int>(pos.y);

		switch (PRIMARYLANGID(inputLang)) {
		case LANG_CHINESE: {
			// As written in a comment in IMM32Manager::CreateImeWindow(),
			// Chinese IMEs ignore function calls to ::ImmSetCandidateWindow()
			// when a user disables TSF (Text Service Framework) and CUAS (Cicero
			// Unaware Application Support).
			// On the other hand, when a user enables TSF and CUAS, Chinese IMEs
			// ignore the position of the current system caret and uses the
			// parameters given to ::ImmSetCandidateWindow() with its 'dwStyle'
			// parameter CFS_CANDIDATEPOS.
			// Therefore, we do not only call ::ImmSetCandidateWindow() but also
			// set the positions of the temporary system caret if it exists.
			CANDIDATEFORM candidatePos = { 0, CFS_CANDIDATEPOS, { x, y }, { 0, 0, 0, 0 } };
			::ImmSetCandidateWindow(hIMC, &candidatePos);
			::SetCaretPos(x, y);
		}
		break;

		case LANG_JAPANESE: {
			// When a user disables TSF (Text Service Framework) and CUAS (Cicero
			// Unaware Application Support), Chinese IMEs somehow ignore function calls
			// to ::ImmSetCandidateWindow(), i.e. they do not move their candidate
			// window to the position given as its parameters, and use the position
			// of the current system caret instead, i.e. it uses ::GetCaretPos() to
			// retrieve the position of their IME candidate window.
			// Therefore, we create a temporary system caret for Chinese IMEs and use
			// it during this input context.
			// Since some third-party Japanese IME also uses ::GetCaretPos() to determine
			// their window position, we also create a caret for Japanese IMEs.
			::SetCaretPos(x, y);
		}
		break;

		case LANG_KOREAN: {
			// Chinese IMEs and Japanese IMEs require the upper-left corner of
			// the caret to move the position of their candidate windows.
			// On the other hand, Korean IMEs require the lower-left corner of the
			// caret to move their candidate windows.
			constexpr int kCaretMargin = 1;
			y += kCaretMargin;
		}
		break;
		}

		// set candidate window under IME indicators.
		// required for Google Chinese IME on Win7.
		const int y2 = y + std::max(4, lineHeight/4);

		// Japanese IMEs and Korean IMEs also use the rectangle given to
		// ::ImmSetCandidateWindow() with its 'dwStyle' parameter CFS_EXCLUDE
		// to move their candidate windows when a user disables TSF and CUAS.
		// Therefore, we also set this parameter here.
		CANDIDATEFORM excludeRect = { 0, CFS_EXCLUDE, { x, y2 }, { x, y, x + sysCaretWidth, y + sysCaretHeight }};
		::ImmSetCandidateWindow(hIMC, &excludeRect);
	}

	void SetCompositionWindow(Point pos) const noexcept {
		COMPOSITIONFORM CompForm{};
		CompForm.dwStyle = CFS_POINT;
		CompForm.ptCurrentPos = POINTFromPoint(pos);
		::ImmSetCompositionWindow(hIMC, &CompForm);
	}

	void SetCompositionFont(const ViewStyle &vs, int style, UINT dpi) const {
		LOGFONTW lf {};
		const int sizeZoomed = GetFontSizeZoomed(vs.styles[style].size, vs.zoomLevel);
		// The negative is to allow for leading
		lf.lfHeight = -::MulDiv(sizeZoomed, dpi, pointsPerInch*FontSizeMultiplier);
		lf.lfWeight = static_cast<LONG>(vs.styles[style].weight);
		lf.lfItalic = vs.styles[style].italic;
		lf.lfCharSet = DEFAULT_CHARSET;
		lf.lfQuality = Win32MapFontQuality(vs.extraFontFlag);
#if 1
		// TODO: change to GetLocaleDefaultUIFont(inputLang, lf.lfFaceName, &dummy) for Vista+.
		memcpy(lf.lfFaceName, defaultTextFontName, sizeof(lf.lfFaceName));
#else
		const char *fontName = vs.styles[style].fontName;
		if (fontName) {
			UTF16FromUTF8(std::string_view(fontName), lf.lfFaceName, LF_FACESIZE);
		}
#endif
		::ImmSetCompositionFontW(hIMC, &lf);
	}

	void Notify(bool complete) const noexcept {
		::ImmNotifyIME(hIMC, NI_COMPOSITIONSTR, complete ? CPS_COMPLETE : CPS_CANCEL, 0);
	}

	LRESULT Escape(HKL hkl, UINT uEscape, LPVOID lpv) const noexcept {
		return ::ImmEscapeW(hkl, hIMC, uEscape, lpv);
	}
};

class GlobalMemory;

class ReverseArrowCursor {
	HCURSOR cursor {};
	bool valid = false;

public:
	ReverseArrowCursor() noexcept = default;
	// Deleted so ReverseArrowCursor objects can not be copied.
	ReverseArrowCursor(const ReverseArrowCursor &) = delete;
	ReverseArrowCursor(ReverseArrowCursor &&) = delete;
	ReverseArrowCursor &operator=(const ReverseArrowCursor &) = delete;
	ReverseArrowCursor &operator=(ReverseArrowCursor &&) = delete;
	~ReverseArrowCursor() {
		if (cursor) {
			::DestroyCursor(cursor);
		}
	}

	void Invalidate() noexcept {
		valid = false;
	}

	HCURSOR Load(UINT dpi) noexcept {
		if (cursor)	{
			if (valid) {
				return cursor;
			}
			::DestroyCursor(cursor);
		}

		valid = true;
		HCURSOR arrow = ::LoadCursor({}, IDC_ARROW);
		cursor = LoadReverseArrowCursor(arrow, dpi);
		return cursor ? cursor : arrow;
	}
};

struct HorizontalScrollRange {
	int pageWidth;
	int documentWidth;
};

}

namespace Scintilla::Internal {

using HwndRenderTarget = ComPtr<ID2D1HwndRenderTarget>;

// There may be either a Hwnd or DC render target
struct RenderTargets {
	HwndRenderTarget pHwndRT;
	DCRenderTarget pDCRT;
#if _WIN32_WINNT >= _WIN32_WINNT_WIN7
	ComPtr<ID2D1DeviceContext> pDeviceContext;
#endif
	bool valid = true;
	[[nodiscard]] ID2D1RenderTarget *RenderTarget() const noexcept {
		if (pHwndRT)
			return pHwndRT.Get();
		if (pDCRT)
			return pDCRT.Get();
#if _WIN32_WINNT >= _WIN32_WINNT_WIN7
		if (pDeviceContext)
			return pDeviceContext.Get();
#endif
		return nullptr;
	}
	void Release() noexcept {
		pHwndRT = nullptr;
		pDCRT = nullptr;
#if _WIN32_WINNT >= _WIN32_WINNT_WIN7
		pDeviceContext = nullptr;
#endif
	}
};

// These resources are device-dependent but not window-dependent
#if _WIN32_WINNT >= _WIN32_WINNT_WIN7
struct DirectDevice {
	D3D11Device pDirect3DDevice;
	ComPtr<ID2D1Device> pDirect2DDevice;
	ComPtr<IDXGIDevice> pDXGIDevice;

	void Release() noexcept;
	HRESULT CreateDevice() noexcept;
};

void DirectDevice::Release() noexcept {
	pDirect3DDevice = nullptr;
	pDirect2DDevice = nullptr;
	pDXGIDevice = nullptr;
}

HRESULT DirectDevice::CreateDevice() noexcept {
	if (pDirect2DDevice) {	// Must be released before creation
		return E_FAIL;
	}

	HRESULT hr = CreateD3D(pDirect3DDevice);
	if (FAILED(hr)) {
		return hr;
	}

	hr = pDirect3DDevice.As(&pDXGIDevice);
	if (FAILED(hr)) {
		return hr;
	}

	hr = pD2DFactory->CreateDevice(pDXGIDevice.Get(), pDirect2DDevice.ReleaseAndGetAddressOf());
	if (FAILED(hr)) {
		return hr;
	}

	return S_OK;
}
#endif // _WIN32_WINNT >= _WIN32_WINNT_WIN7

/**
 */
class ScintillaWin final :
	public ScintillaBase {

	bool lastKeyDownConsumed = false;
	bool styleIdleInQueue = false;
	wchar_t lastHighSurrogateChar = 0;

	bool capturedMouse = false;
	bool trackedMouseLeave = false;
	bool cursorIsHidden = false;
	bool hasOKText = false;
#if _WIN32_WINNT < _WIN32_WINNT_WIN8
	SetCoalescableTimerSig SetCoalescableTimerFn = nullptr;
#endif

	BOOL typingWithoutCursor = FALSE;
	UINT linesPerScroll = 0;	///< Intellimouse support
	UINT charsPerScroll = 0;	///< Intellimouse support
	MouseWheelDelta verticalWheelDelta;
	MouseWheelDelta horizontalWheelDelta;

	UINT dpi = USER_DEFAULT_SCREEN_DPI;
	ReverseArrowCursor reverseArrowCursor;

	PRectangle rectangleClient;
	HRGN hRgnUpdate {};

	CLIPFORMAT cfColumnSelect;
	CLIPFORMAT cfBorlandIDEBlockType;
	CLIPFORMAT cfLineSelect;
	CLIPFORMAT cfVSLineTag;

#if EnableDrop_VisualStudioProjectItem
	CLIPFORMAT cfVSStgProjectItem;
	CLIPFORMAT cfVSRefProjectItem;
#endif
#if Enable_ChromiumWebCustomMIMEDataFormat
	CLIPFORMAT cfChromiumCustomMIME;
#endif
#if DebugCopyAsRichTextFormat
	CLIPFORMAT cfRTF;
#endif

	// supported drag & drop format
	CLIPFORMAT dropFormat[MaxDragAndDropDataFormatCount];
	UINT dropFormatCount;

	DropSource ds;
	DataObject dob;
	DropTarget dt;

	static HINSTANCE hInstance;
	static ATOM scintillaClassAtom;
	static ATOM callClassAtom;

	// The current input Language ID.
	LANGID inputLang = LANG_USER_DEFAULT;

#if _WIN32_WINNT >= _WIN32_WINNT_WIN7
	DirectDevice device;
	ComPtr<IDXGISwapChain1> pDXGISwapChain;
#endif
	RenderTargets targets;
	// rendering parameters for current monitor
	HMONITOR hCurrentMonitor {};
	WriteRenderingParams defaultRenderingParams;
	WriteRenderingParams customRenderingParams;

	explicit ScintillaWin(HWND hwnd) noexcept;
	// ~ScintillaWin() in public section

	void Finalise() noexcept override;
	void SetRenderingParams(Surface *surface) const noexcept {
		surface->SetRenderingParams(defaultRenderingParams.Get(), customRenderingParams.Get());
	}
	bool UpdateRenderingParams(bool force) noexcept;
	void CreateRenderTarget() noexcept;
#if _WIN32_WINNT >= _WIN32_WINNT_WIN7
	HRESULT Create3D() noexcept;
	HRESULT SetBackBuffer(IDXGISwapChain1 *pSwapChain) const noexcept;
	HRESULT CreateSwapChain(HWND hwnd) noexcept;
#endif
	void EnsureRenderTarget(HDC hdc) noexcept;
	void DropRenderTarget() noexcept {
		targets.Release();
	}
	[[nodiscard]] HWND MainHWND() const noexcept {
		return HwndFromWindow(wMain);
	}
#if DebugDragAndDropDataFormat
	void EnumDataSourceFormat(const char *tag, LPDATAOBJECT pIDataSource);
	void EnumAllClipboardFormat(const char *tag);
#else
	#define EnumDataSourceFormat(tag, pIDataSource)
	#define EnumAllClipboardFormat(tag)
#endif

	//static sptr_t DirectFunction(sptr_t ptr, UINT iMessage, uptr_t wParam, sptr_t lParam);
	//static sptr_t DirectStatusFunction(sptr_t ptr, UINT iMessage, uptr_t wParam, sptr_t lParam, int *pStatus);
	static LRESULT CALLBACK SWndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam);

	void CTPaint(HWND hWnd);
	LRESULT CTProcessMessage(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam);
	static LRESULT CALLBACK CTWndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam);

	enum : UINT_PTR {
		invalidTimerID, standardTimerID, idleTimerID, fineTimerStart
	};

	void DisplayCursor(Window::Cursor c) noexcept override;
	bool SCICALL DragThreshold(Point ptStart, Point ptNow) noexcept override;
	void StartDrag() override;
	static KeyMod MouseModifiers(uptr_t wParam) noexcept;

	Sci::Position TargetAsUTF8(char *text) const;
	Sci::Position EncodedFromUTF8(const char *utf8, char *encoded) const;

	bool PaintDC(HDC hdc);
	sptr_t WndPaint();

	// DBCS
	void ImeStartComposition();
	void ImeEndComposition();
	LRESULT ImeOnReconvert(LPARAM lParam);
	[[nodiscard]] LRESULT ImeOnDocumentFeed(LPARAM lParam) const;
	sptr_t HandleCompositionWindowed(uptr_t wParam, sptr_t lParam);
	sptr_t HandleCompositionInline(uptr_t wParam, sptr_t lParam);

	// Korean IME always use inline mode, and use block caret in inline mode.
	bool KoreanIME() const noexcept {
		return PRIMARYLANGID(inputLang) == LANG_KOREAN;
	}

	void SetCandidateWindowPos(const IMContext &imc);
	void SelectionToHangul();
	void EscapeHanja();
	void ToggleHanja();
	void AddWString(std::wstring_view wsv, CharacterSource charSource);
	bool HandleLaTeXTabCompletion();

	[[nodiscard]] UINT CodePageOfDocument() const noexcept;
	[[nodiscard]] bool ValidCodePage(int codePage) const noexcept override;
	[[nodiscard]] std::string UTF8FromEncoded(std::string_view encoded) const override;
	[[nodiscard]] std::string EncodedFromUTF8(std::string_view utf8) const override;

	std::string EncodeWString(std::wstring_view wsv) const;
	sptr_t DefWndProc(Message iMessage, uptr_t wParam, sptr_t lParam) noexcept override;
	void IdleWork() override;
	void QueueIdleWork(WorkItems items, Sci::Position upTo) noexcept override;
	bool SetIdle(bool on) noexcept override;
	UINT_PTR timers[static_cast<int>(TickReason::dwell) + 1]{};
	bool FineTickerRunning(TickReason reason) const noexcept override;
	void FineTickerStart(TickReason reason, int millis, int tolerance) noexcept override;
	void FineTickerCancel(TickReason reason) noexcept override;
	void SetMouseCapture(bool on) noexcept override;
	bool HaveMouseCapture() const noexcept override;
	void SetTrackMouseLeaveEvent(bool on) noexcept;
	void HideCursorIfPreferred() noexcept;
	void UpdateBaseElements() noexcept override;
	bool SCICALL PaintContains(PRectangle rc) const noexcept override;
	void ScrollText(Sci::Line linesToMove) override;
	void NotifyCaretMove() noexcept override;
	void UpdateSystemCaret() override;
	void SetVerticalScrollPos() override;
	void SetHorizontalScrollPos() override;
	void HorizontalScrollToClamped(int xPos);
	[[nodiscard]] HorizontalScrollRange GetHorizontalScrollRange() const noexcept;
	bool ModifyScrollBars(Sci::Line nMax, Sci::Line nPage) override;
	void NotifyChange() noexcept override;
	void NotifyFocus(bool focus) override;
	void SetCtrlID(int identifier) noexcept override;
	int GetCtrlID() const noexcept override;
	void NotifyParent(NotificationData &scn) const noexcept override;
	void NotifyDoubleClick(Point pt, KeyMod modifiers) override;
	std::unique_ptr<CaseFolder> CaseFolderForEncoding() override;
	std::string CaseMapString(const std::string &s, CaseMapping caseMapping) const override;
	void Copy(bool asBinary) const override;
	bool CanPaste() noexcept override;
	void Paste(bool asBinary) override;
	void SCICALL CreateCallTipWindow(PRectangle rc) noexcept override;
#if SCI_EnablePopupMenu
	void AddToPopUp(const char *label, int cmd = 0, bool enabled = true) noexcept override;
#endif
	void ClaimSelection() noexcept override;

	enum class CopyEncoding {
		Unicode,	// used in Copy & Paste, Drag & Drop
		Ansi,		// used in Drag & Drop for CF_TEXT
		Binary,		// used in Copy & Paste for asBinary
	};

	void GetMouseParameters() noexcept;
	void CopyToGlobal(GlobalMemory &gmUnicode, const SelectionText &selectedText, CopyEncoding encoding) const;
	void CopyToClipboard(const SelectionText &selectedText) const override;
	void ScrollMessage(WPARAM wParam);
	void HorizontalScrollMessage(WPARAM wParam);
	void FullPaint();
	void FullPaintDC(HDC hdc);
	bool IsCompatibleDC(HDC hOtherDC) const noexcept;
	[[nodiscard]] DWORD EffectFromState(DWORD grfKeyState) const noexcept;

	[[nodiscard]] BOOL IsVisible() const noexcept;
	int SetScrollInfo(int nBar, LPCSCROLLINFO lpsi, BOOL bRedraw) const noexcept;
	bool GetScrollInfo(int nBar, LPSCROLLINFO lpsi) const noexcept;
	bool ChangeScrollRange(int nBar, int nMin, int nMax, UINT nPage) const noexcept;
	void ChangeScrollPos(int barType, Sci::Position pos);
	sptr_t GetTextLength() const noexcept;
	sptr_t GetText(uptr_t wParam, sptr_t lParam) const;
	Window::Cursor SCICALL ContextCursor(Point pt);
#if SCI_EnablePopupMenu
	sptr_t ShowContextMenu(unsigned int iMessage, uptr_t wParam, sptr_t lParam);
#endif
	[[nodiscard]] PRectangle GetClientRectangle() const noexcept override {
		return rectangleClient;
	}
	void SizeWindow();
	sptr_t MouseMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam);
	sptr_t KeyMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam);
	sptr_t FocusMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam);
	sptr_t IMEMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam);
	sptr_t EditMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam);
	sptr_t IdleMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam);
	sptr_t SciMessage(Message iMessage, uptr_t wParam, sptr_t lParam);

public:
	// Deleted so ScintillaWin objects can not be copied.
	ScintillaWin(const ScintillaWin &) = delete;
	ScintillaWin(ScintillaWin &&) = delete;
	ScintillaWin &operator=(const ScintillaWin &) = delete;
	ScintillaWin &operator=(ScintillaWin &&) = delete;
	~ScintillaWin() override;

	// Public for benefit of Scintilla_DirectFunction
	sptr_t WndProc(Message iMessage, uptr_t wParam, sptr_t lParam) override;

	/// Implement IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, PVOID *ppv) noexcept;
	STDMETHODIMP_(ULONG)AddRef() noexcept;
	STDMETHODIMP_(ULONG)Release() noexcept;

	/// Implement IDropTarget
	STDMETHODIMP DragEnter(LPDATAOBJECT pIDataSource, DWORD grfKeyState,
		POINTL pt, PDWORD pdwEffect);
	STDMETHODIMP DragOver(DWORD grfKeyState, POINTL pt, PDWORD pdwEffect);
	STDMETHODIMP DragLeave();
	STDMETHODIMP Drop(LPDATAOBJECT pIDataSource, DWORD grfKeyState,
		POINTL pt, PDWORD pdwEffect);

	/// Implement important part of IDataObject
	STDMETHODIMP GetData(const FORMATETC *pFEIn, STGMEDIUM *pSTM);

#if USE_WIN32_INIT_ONCE
	static BOOL CALLBACK PrepareOnce(PINIT_ONCE initOnce, PVOID parameter, PVOID *lpContext) noexcept;
#else
	static void PrepareOnce() noexcept;
#endif
	static bool Register(HINSTANCE hInstance_) noexcept;
	static bool Unregister() noexcept;

	[[nodiscard]] bool DragIsRectangularOK(UINT fmt) const noexcept {
		return drag.rectangular && (fmt == cfColumnSelect);
	}

private:
	// For use in creating a system caret
	[[nodiscard]] bool HasCaretSizeChanged() const noexcept;
	BOOL CreateSystemCaret();
	BOOL DestroySystemCaret() noexcept;
	HBITMAP sysCaretBitmap {};
	int sysCaretWidth = 0;
	int sysCaretHeight = 0;
};

HINSTANCE ScintillaWin::hInstance {};
ATOM ScintillaWin::scintillaClassAtom = 0;
ATOM ScintillaWin::callClassAtom = 0;

ScintillaWin::ScintillaWin(HWND hwnd) noexcept {
	wMain = hwnd;
	dpi = GetWindowDPI(hwnd);

	// There does not seem to be a real standard for indicating that the clipboard
	// contains a rectangular selection, so copy Developer Studio and Borland Delphi.
	cfColumnSelect = RegisterClipboardType(L"MSDEVColumnSelect");
	cfBorlandIDEBlockType = RegisterClipboardType(L"Borland IDE Block Type");

	// Likewise for line-copy or line-cut (copies or cuts a full line when no text is selected)
	cfLineSelect = RegisterClipboardType(L"MSDEVLineSelect");
	cfVSLineTag = RegisterClipboardType(L"VisualStudioEditorOperationsLineCutCopyClipboardTag");

#if EnableDrop_VisualStudioProjectItem
	cfVSStgProjectItem = RegisterClipboardType(L"CF_VSSTGPROJECTITEMS");
	cfVSRefProjectItem = RegisterClipboardType(L"CF_VSREFPROJECTITEMS");
#endif
#if Enable_ChromiumWebCustomMIMEDataFormat
	cfChromiumCustomMIME = RegisterClipboardType(L"Chromium Web Custom MIME Data Format");
#endif
#if DebugCopyAsRichTextFormat
	cfRTF = RegisterClipboardType(L"Rich Text Format");
#endif

	UINT index = 0;
#if defined(_WIN64) && (_WIN32_WINNT < _WIN32_WINNT_WIN10)
	dropFormat[index++] = CF_HDROP;
#endif
#if EnableDrop_VisualStudioProjectItem
	dropFormat[index++] = cfVSStgProjectItem;
	dropFormat[index++] = cfVSRefProjectItem;
#endif
#if Enable_ChromiumWebCustomMIMEDataFormat
	dropFormat[index++] = cfChromiumCustomMIME;
#endif
	// text format comes last
	dropFormat[index++] = CF_UNICODETEXT;
	dropFormat[index++] = CF_TEXT;
	dropFormatCount = index;

	dob.sci = this;
	ds.sci = this;
	dt.sci = this;

	caret.period = ::GetCaretBlinkTime();
	if (caret.period < 0) {
		caret.period = 0;
	}

	// Find SetCoalescableTimer which is only available from Windows 8+
#if _WIN32_WINNT < _WIN32_WINNT_WIN8
	SetCoalescableTimerFn = DLLFunctionEx<SetCoalescableTimerSig>(L"user32.dll", "SetCoalescableTimer");
#endif

	vs.indicators[IndicatorUnknown] = Indicator(IndicatorStyle::Hidden, colourIME);
	vs.indicators[IndicatorInput] = Indicator(IndicatorStyle::Dots, colourIME);
	vs.indicators[IndicatorConverted] = Indicator(IndicatorStyle::CompositionThick, colourIME);
	vs.indicators[IndicatorTarget] = Indicator(IndicatorStyle::StraightBox, colourIME);
}

ScintillaWin::~ScintillaWin() {
	if (sysCaretBitmap) {
		::DeleteObject(sysCaretBitmap);
	}
}

void ScintillaWin::Finalise() noexcept {
	ScintillaBase::Finalise();
	for (TickReason tr = TickReason::caret; tr <= TickReason::dwell;
		tr = static_cast<TickReason>(static_cast<int>(tr) + 1)) {
		FineTickerCancel(tr);
	}
	SetIdle(false);
	DropRenderTarget();
	::RevokeDragDrop(MainHWND());
}

namespace {

HRESULT CreateHwndRenderTarget(const D2D1_RENDER_TARGET_PROPERTIES *renderTargetProperties,
	const D2D1_HWND_RENDER_TARGET_PROPERTIES *hwndRenderTargetProperties, HwndRenderTarget &hwndRT) noexcept {
	return pD2DFactory->CreateHwndRenderTarget(renderTargetProperties, hwndRenderTargetProperties, hwndRT.ReleaseAndGetAddressOf());
}

}

bool ScintillaWin::UpdateRenderingParams(bool force) noexcept {
	// see https://sourceforge.net/p/scintilla/bugs/2344/?page=2
	//HWND topLevel = ::GetAncestor(MainHWND(), GA_ROOT);
	HWND topLevel = ::GetParent(MainHWND()); // our main window
	HMONITOR monitor = ::MonitorFromWindow(topLevel, MONITOR_DEFAULTTONEAREST);
	if (!force && monitor == hCurrentMonitor && (technology == Technology::Default || defaultRenderingParams)) {
		return false;
	}

	WriteRenderingParams monitorRenderingParams;
	WriteRenderingParams customClearTypeRenderingParams;
	if (technology != Technology::Default) {
#if _WIN32_WINNT >= _WIN32_WINNT_WIN7
		ComPtr<IDWriteRenderingParams> upMrp;
		HRESULT hr = pIDWriteFactory->CreateMonitorRenderingParams(monitor, upMrp.GetAddressOf());
		if (SUCCEEDED(hr)) {
			// Cast to IDWriteRenderingParams1 so can call GetGrayscaleEnhancedContrast()
			hr = upMrp.As(&monitorRenderingParams);
		}
#else
		const HRESULT hr = pIDWriteFactory->CreateMonitorRenderingParams(monitor, monitorRenderingParams.GetAddressOf());
#endif
		UINT clearTypeContrast = 0;
		if (SUCCEEDED(hr) && monitorRenderingParams &&
			::SystemParametersInfo(SPI_GETFONTSMOOTHINGCONTRAST, 0, &clearTypeContrast, 0) != 0) {
			if (clearTypeContrast >= 1000 && clearTypeContrast <= 2200) {
				const FLOAT gamma = static_cast<FLOAT>(clearTypeContrast) / 1000.0f;
				pIDWriteFactory->CreateCustomRenderingParams(gamma,
					monitorRenderingParams->GetEnhancedContrast(),
#if _WIN32_WINNT >= _WIN32_WINNT_WIN7
					monitorRenderingParams->GetGrayscaleEnhancedContrast(),
#endif
					monitorRenderingParams->GetClearTypeLevel(),
					monitorRenderingParams->GetPixelGeometry(),
					monitorRenderingParams->GetRenderingMode(),
					customClearTypeRenderingParams.GetAddressOf());
			}
		}
	}

	hCurrentMonitor = monitor;
	defaultRenderingParams = std::move(monitorRenderingParams);
	customRenderingParams = std::move(customClearTypeRenderingParams);
	return true;
}

#if _WIN32_WINNT >= _WIN32_WINNT_WIN7
HRESULT ScintillaWin::Create3D() noexcept {
	if (device.pDirect2DDevice) {
		return S_OK;
	}
	targets.Release();
	pDXGISwapChain = nullptr;
	device.Release();
	const HRESULT hr = device.CreateDevice();
	if (FAILED(hr)) {
		device.Release();
	}
	return hr;
}
#endif

void ScintillaWin::CreateRenderTarget() noexcept {
	HWND hw = MainHWND();

	// Create a Direct2D render target.

	if (technology == Technology::DirectWriteDC) {
		const D2D1_RENDER_TARGET_PROPERTIES drtp = D2D1::RenderTargetProperties(
			D2D1_RENDER_TARGET_TYPE_DEFAULT,
			{ DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE },
			dpiDefault, dpiDefault);

		const HRESULT hr = CreateDCRenderTarget(&drtp, targets.pDCRT);
		if (FAILED(hr)) {
			// Platform::DebugPrintf("Failed CreateDCRenderTarget 0x%lx\n", hr);
			targets.Release();
		} else {
			//D2D1::Matrix3x2F invertX = D2D1::Matrix3x2F(-1, 0, 0, 1, 0, 0);
			//D2D1::Matrix3x2F moveX = D2D1::Matrix3x2F::Translation(rc.right - rc.left, 0);
			//pRenderTarget->SetTransform(invertX * moveX);
		}
	}
#if _WIN32_WINNT >= _WIN32_WINNT_WIN7
	else if (technology == Technology::DirectWrite1) {
		HRESULT hr = Create3D();	// Need pDirect2DDevice
		if (SUCCEEDED(hr)) {
			hr = device.pDirect2DDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE,
				targets.pDeviceContext.ReleaseAndGetAddressOf());
			if (FAILED(hr)) {
				// Platform::DebugPrintf("Failed CreateDeviceContext 0x%lx\n", hr);
			} else {
				hr = CreateSwapChain(hw);
				if (FAILED(hr)) {
					// Platform::DebugPrintf("Failed CreateSwapChain 0x%lx\n", hr);
					targets.Release();
				}
			}
		}
	}
#endif
	else { // DirectWrite or DirectWriteRetain
		const RECT rc = GetClientRect(hw);

		const D2D1_RENDER_TARGET_PROPERTIES drtp = D2D1::RenderTargetProperties(
			D2D1_RENDER_TARGET_TYPE_DEFAULT,
			{ DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN },
			dpiDefault, dpiDefault);

		const D2D1_PRESENT_OPTIONS presentOptions = (technology == Technology::DirectWriteRetain) ?
			D2D1_PRESENT_OPTIONS_RETAIN_CONTENTS : D2D1_PRESENT_OPTIONS_NONE;

		const D2D1_HWND_RENDER_TARGET_PROPERTIES dhrtp = D2D1::HwndRenderTargetProperties(
			hw,
			GetSizeUFromRect(rc),
			presentOptions);

		const HRESULT hr = CreateHwndRenderTarget(&drtp, &dhrtp, targets.pHwndRT);
		if (FAILED(hr)) {
			// Platform::DebugPrintf("Failed CreateHwndRenderTarget 0x%lx\n", hr);
			targets.Release();
		}
	}
}

#if _WIN32_WINNT >= _WIN32_WINNT_WIN7
HRESULT ScintillaWin::SetBackBuffer(IDXGISwapChain1 *pSwapChain) const noexcept {
	assert(targets.pDeviceContext);
	// Back buffer as an IDXGISurface
	ComPtr<IDXGISurface> dxgiBackBuffer;
	HRESULT hr = pSwapChain->GetBuffer(0, IID_PPV_ARGS(dxgiBackBuffer.GetAddressOf()));
	if (FAILED(hr))
		return hr;

	const FLOAT dpiX = static_cast<FLOAT>(static_cast<int>(dpi));

	// Direct2D bitmap linked to Direct3D texture through DXGI back buffer
	const D2D1_BITMAP_PROPERTIES1 bitmapProperties =
		D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
			D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), dpiX, dpiX);
	ComPtr<ID2D1Bitmap1> pDirect2DBackBuffer;
	hr = targets.pDeviceContext->CreateBitmapFromDxgiSurface(dxgiBackBuffer.Get(), &bitmapProperties, pDirect2DBackBuffer.GetAddressOf());
	if (FAILED(hr))
		return hr;

	// Bitmap is render target
	targets.pDeviceContext->SetTarget(pDirect2DBackBuffer.Get());

	return S_OK;
}

HRESULT ScintillaWin::CreateSwapChain(HWND hwnd) noexcept {
	// Sets pDXGISwapChain but only when each call succeeds
	// Needs pDXGIDevice, pDirect3DDevice
	pDXGISwapChain = nullptr;
	assert(device.pDXGIDevice);

	// At each stage, place object in a unique_ptr to ensure release occurs

	ComPtr<IDXGIAdapter> dxgiAdapter;
	HRESULT hr = device.pDXGIDevice->GetAdapter(dxgiAdapter.GetAddressOf());
	if (FAILED(hr))
		return hr;

	ComPtr<IDXGIFactory2> dxgiFactory;
	hr = dxgiAdapter->GetParent(IID_PPV_ARGS(dxgiFactory.GetAddressOf()));
	if (FAILED(hr))
		return hr;

	DXGI_SWAP_CHAIN_DESC1 swapChainDesc{};
	swapChainDesc.Width = 0;
	swapChainDesc.Height = 0;
	swapChainDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	swapChainDesc.Stereo = false;
	swapChainDesc.SampleDesc.Count = 1;
	swapChainDesc.SampleDesc.Quality = 0;
	swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	swapChainDesc.BufferCount = 2;
	swapChainDesc.Scaling = DXGI_SCALING_STRETCH;
	swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
	swapChainDesc.Flags = 0;

	// DXGI swap chain for window
	ComPtr<IDXGISwapChain1> pSwapChain;
	hr = dxgiFactory->CreateSwapChainForHwnd(device.pDirect3DDevice.Get(), hwnd, &swapChainDesc,
		nullptr, nullptr, pSwapChain.GetAddressOf());
	if (FAILED(hr))
		return hr;

	hr = SetBackBuffer(pSwapChain.Get());
	if (FAILED(hr))
		return hr;

	// All successful so export swap chain for later presentation
	pDXGISwapChain = std::move(pSwapChain);
	return S_OK;
}
#endif // _WIN32_WINNT >= _WIN32_WINNT_WIN7

void ScintillaWin::EnsureRenderTarget(HDC hdc) noexcept {
	if (!targets.valid) {
		DropRenderTarget();
		targets.valid = true;
	}
	if (!targets.RenderTarget()) {
		CreateRenderTarget();
		// Pixmaps were created to be compatible with previous render target so
		// need to be recreated.
		DropGraphics();
	}

	if ((technology == Technology::DirectWriteDC) && targets.pDCRT) {	// DC RenderTarget needs binding
		const RECT rcWindow = GetClientRect(MainHWND());
		const HRESULT hr = targets.pDCRT->BindDC(hdc, &rcWindow);
		if (FAILED(hr)) {
			//Platform::DebugPrintf("BindDC failed 0x%lx\n", hr);
			DropRenderTarget();
		}
	}
}

void ScintillaWin::DisplayCursor(Window::Cursor c) noexcept {
	if (cursorMode != CursorShape::Normal) {
		c = static_cast<Window::Cursor>(cursorMode);
	}
	if (c == Window::Cursor::reverseArrow) {
		::SetCursor(reverseArrowCursor.Load(dpi));
	} else {
		wMain.SetCursor(c);
	}
}

bool ScintillaWin::DragThreshold(Point ptStart, Point ptNow) noexcept {
	const Point ptDifference = ptStart - ptNow;
	const XYPOSITION xMove = std::trunc(std::abs(ptDifference.x));
	const XYPOSITION yMove = std::trunc(std::abs(ptDifference.y));
	return (xMove > SystemMetricsForDpi(SM_CXDRAG, dpi)) ||
		(yMove > SystemMetricsForDpi(SM_CYDRAG, dpi));
}

void ScintillaWin::StartDrag() {
	inDragDrop = DragDrop::dragging;
	DWORD dwEffect = 0;
	dropWentOutside = true;
	IDataObject *pDataObject = &dob;
	IDropSource *pDropSource = &ds;
	//Platform::DebugPrintf("About to DoDragDrop %p %p\n", pDataObject, pDropSource);
	const HRESULT hr = ::DoDragDrop(
		pDataObject,
		pDropSource,
		DROPEFFECT_COPY | DROPEFFECT_MOVE, &dwEffect);
	//Platform::DebugPrintf("DoDragDrop = %lx\n", hr);
	if (SUCCEEDED(hr)) {
		if ((hr == DRAGDROP_S_DROP) && (dwEffect == DROPEFFECT_MOVE) && dropWentOutside) {
			// Remove dragged out text
			ClearSelection();
		}
	}
	inDragDrop = DragDrop::none;
	SetDragPosition(SelectionPosition(Sci::invalidPosition));
}

KeyMod ScintillaWin::MouseModifiers(uptr_t wParam) noexcept {
	return ModifierFlags((wParam & MK_SHIFT) != 0,
		(wParam & MK_CONTROL) != 0,
		KeyboardIsKeyDown(VK_MENU));
}

}

namespace {

/** Map the key codes to their equivalent Keys:: form. */
constexpr Keys KeyTranslate(uptr_t keyIn) noexcept {
	switch (keyIn) {
	case VK_DOWN:		return Keys::Down;
	case VK_UP:			return Keys::Up;
	case VK_LEFT:		return Keys::Left;
	case VK_RIGHT:		return Keys::Right;
	case VK_HOME:		return Keys::Home;
	case VK_END:		return Keys::End;
	case VK_PRIOR:		return Keys::Prior;
	case VK_NEXT:		return Keys::Next;
	case VK_DELETE:		return Keys::Delete;
	case VK_INSERT:		return Keys::Insert;
	case VK_ESCAPE:		return Keys::Escape;
	case VK_BACK:		return Keys::Back;
	case VK_TAB:		return Keys::Tab;
	case VK_RETURN:		return Keys::Return;
	case VK_ADD:		return Keys::Add;
	case VK_SUBTRACT:	return Keys::Subtract;
	case VK_DIVIDE:		return Keys::Divide;
	case VK_LWIN:		return Keys::Win;
	case VK_RWIN:		return Keys::RWin;
	case VK_APPS:		return Keys::Menu;
	case VK_OEM_2:		return static_cast<Keys>('/');
	case VK_OEM_3:		return static_cast<Keys>('`');
	case VK_OEM_4:		return static_cast<Keys>('[');
	case VK_OEM_5:		return static_cast<Keys>('\\');
	case VK_OEM_6:		return static_cast<Keys>(']');
	default:			return static_cast<Keys>(keyIn);
	}
}

bool BoundsContains(PRectangle rcBounds, HRGN hRgnBounds, PRectangle rcCheck) noexcept {
	bool contains = true;
	if (!rcCheck.Empty()) {
		if (!rcBounds.Contains(rcCheck)) {
			contains = false;
		} else if (hRgnBounds) {
			// In bounding rectangle so check more accurately using region
			const RECT rcw = RectFromPRectangleEx(rcCheck);
			HRGN hRgnCheck = ::CreateRectRgnIndirect(&rcw);
			if (hRgnCheck) {
				HRGN hRgnDifference = ::CreateRectRgn(0, 0, 0, 0);
				if (hRgnDifference) {
					const int combination = ::CombineRgn(hRgnDifference, hRgnCheck, hRgnBounds, RGN_DIFF);
					if (combination != NULLREGION) {
						contains = false;
					}
					::DeleteRgn(hRgnDifference);
				}
				::DeleteRgn(hRgnCheck);
			}
		}
	}
	return contains;
}

// Simplify calling WideCharToMultiByte and MultiByteToWideChar by providing default parameters and using string view.

inline int MultiByteFromWideChar(UINT codePage, std::wstring_view wsv, LPSTR lpMultiByteStr, ptrdiff_t cbMultiByte) noexcept {
	return ::WideCharToMultiByte(codePage, 0, wsv.data(), static_cast<int>(wsv.length()), lpMultiByteStr, static_cast<int>(cbMultiByte), nullptr, nullptr);
}

inline int MultiByteLenFromWideChar(UINT codePage, std::wstring_view wsv) noexcept {
	return MultiByteFromWideChar(codePage, wsv, nullptr, 0);
}

inline int WideCharFromMultiByte(UINT codePage, std::string_view sv, LPWSTR lpWideCharStr, ptrdiff_t cchWideChar) noexcept {
	return ::MultiByteToWideChar(codePage, 0, sv.data(), static_cast<int>(sv.length()), lpWideCharStr, static_cast<int>(cchWideChar));
}

inline int WideCharLenFromMultiByte(UINT codePage, std::string_view sv) noexcept {
	return WideCharFromMultiByte(codePage, sv, nullptr, 0);
}

std::string StringEncode(const std::wstring_view wsv, int codePage) {
	const int cchMulti = wsv.empty() ? 0: MultiByteLenFromWideChar(codePage, wsv);
	std::string sMulti(cchMulti, '\0');
	if (cchMulti) {
		MultiByteFromWideChar(codePage, wsv, sMulti.data(), cchMulti);
	}
	return sMulti;
}

std::wstring StringDecode(const std::string_view sv, int codePage) {
	const int cchWide = sv.empty() ? 0: WideCharLenFromMultiByte(codePage, sv);
	std::wstring sWide(cchWide, 0);
	if (cchWide) {
		WideCharFromMultiByte(codePage, sv, sWide.data(), cchWide);
	}
	return sWide;
}

std::wstring StringMapCase(const std::wstring_view wsv, DWORD mapFlags) {
#if _WIN32_WINNT >= _WIN32_WINNT_VISTA
	const int charsConverted = ::LCMapStringEx(LOCALE_NAME_USER_DEFAULT, mapFlags,
		wsv.data(), static_cast<int>(wsv.length()), nullptr, 0, nullptr, nullptr, 0);
	std::wstring wsConverted(charsConverted, 0);
	if (charsConverted) {
		::LCMapStringEx(LOCALE_NAME_USER_DEFAULT, mapFlags,
			wsv.data(), static_cast<int>(wsv.length()), wsConverted.data(), charsConverted, nullptr, nullptr, 0);
	}
#else
	const int charsConverted = ::LCMapStringW(LOCALE_USER_DEFAULT, mapFlags,
		wsv.data(), static_cast<int>(wsv.length()), nullptr, 0);
	std::wstring wsConverted(charsConverted, 0);
	if (charsConverted) {
		::LCMapStringW(LOCALE_USER_DEFAULT, mapFlags,
			wsv.data(), static_cast<int>(wsv.length()), wsConverted.data(), charsConverted);
	}
#endif
	return wsConverted;
}

#if 0
const char *GetCurrentLogTime() {
	static char buf[16];
	SYSTEMTIME lt;
	GetLocalTime(&lt);
	sprintf(buf, "%02d:%02d:%02d.%03d", lt.wHour, lt.wMinute, lt.wSecond, lt.wMilliseconds);
	return buf;
}
#endif

}

// Returns the target converted to UTF8.
// Return the length in bytes.
Sci::Position ScintillaWin::TargetAsUTF8(char *text) const {
	const Sci::Position targetLength = targetRange.Length();
	if (IsUnicodeMode()) {
		if (text) {
			pdoc->GetCharRange(text, targetRange.start.Position(), targetLength);
		}
	} else {
		// Need to convert
		const std::string s = RangeText(targetRange.start.Position(), targetRange.end.Position());
		const std::wstring characters = StringDecode(s, CodePageOfDocument());
		const int utf8Len = MultiByteLenFromWideChar(CpUtf8, characters);
		if (text) {
			MultiByteFromWideChar(CpUtf8, characters, text, utf8Len);
			text[utf8Len] = '\0';
		}
		return utf8Len;
	}
	return targetLength;
}

// Translates a nul terminated UTF8 string into the document encoding.
// Return the length of the result in bytes.
Sci::Position ScintillaWin::EncodedFromUTF8(const char *utf8, char *encoded) const {
	const Sci::Position inputLength = (lengthForEncode >= 0) ? lengthForEncode : strlen(utf8);
	if (IsUnicodeMode()) {
		if (encoded) {
			memcpy(encoded, utf8, inputLength);
		}
		return inputLength;
	}
	// Need to convert
	const std::string_view utf8Input(utf8, inputLength);
	const int charsLen = WideCharLenFromMultiByte(CpUtf8, utf8Input);
	std::wstring characters(charsLen, L'\0');
	WideCharFromMultiByte(CpUtf8, utf8Input, characters.data(), charsLen);

	const UINT codePage = CodePageOfDocument();
	const int encodedLen = MultiByteLenFromWideChar(codePage, characters);
	if (encoded) {
		MultiByteFromWideChar(codePage, characters, encoded, encodedLen);
		encoded[encodedLen] = '\0';
	}
	return encodedLen;
}

bool ScintillaWin::PaintDC(HDC hdc) {
	//printf("%s %s\n", GetCurrentLogTime(), __func__);
	if (technology == Technology::Default) {
		const AutoSurface surfaceWindow(hdc, this);
		if (surfaceWindow) {
			Paint(surfaceWindow, rcPaint);
			surfaceWindow->Release();
		}
	} else {
		// RefreshStyleData may set scroll bars and resize the window.
		// Avoid issues resizing inside Paint when calling IDXGISwapChain1->ResizeBuffers
		// with committed resources by refreshing the style data first.
		RefreshStyleData();

		//SetLayout(hdc, LAYOUT_BITMAPORIENTATIONPRESERVED);
		EnsureRenderTarget(hdc);
		if (ID2D1RenderTarget *pRenderTarget = targets.RenderTarget()) {
			const AutoSurface surfaceWindow(pRenderTarget, this);
			if (surfaceWindow) {
				SetRenderingParams(surfaceWindow);
				pRenderTarget->BeginDraw();
				Paint(surfaceWindow, rcPaint);
				surfaceWindow->Release();
				const HRESULT hr = pRenderTarget->EndDraw();
				if (hr == static_cast<HRESULT>(D2DERR_RECREATE_TARGET)) {
					DropRenderTarget();
					return false;
				}
#if _WIN32_WINNT >= _WIN32_WINNT_WIN7
				if ((technology == Technology::DirectWrite1) && pDXGISwapChain) {
					const DXGI_PRESENT_PARAMETERS parameters{};
					const HRESULT hrPresent = pDXGISwapChain->Present1(1, 0, &parameters);
					if (FAILED(hrPresent)) {
						DropRenderTarget();
						return false;
					}
				}
#endif
			}
		}
	}

	return true;
}

sptr_t ScintillaWin::WndPaint() {
	//const ElapsedPeriod ep;

	// Redirect assertions to debug output and save current state
	//const bool assertsPopup = Platform::ShowAssertionPopUps(false);
	paintState = PaintState::painting;

	// Removed since this interferes with reporting other assertions as it occurs repeatedly
	//PLATFORM_ASSERT(hRgnUpdate == nullptr);
	hRgnUpdate = ::CreateRectRgn(0, 0, 0, 0);
	::GetUpdateRgn(MainHWND(), hRgnUpdate, FALSE);
	{
		const Painter painter(MainHWND());
		rcPaint = PRectangleFromRectEx(painter.ps.rcPaint);
		const PRectangle rcClient = GetClientRectangle();
		paintingAllText = BoundsContains(rcPaint, hRgnUpdate, rcClient);
		if (!PaintDC(painter.ps.hdc)) {
			paintState = PaintState::abandoned;
		}
	}
	if (hRgnUpdate) {
		::DeleteRgn(hRgnUpdate);
		hRgnUpdate = {};
	}

	if (paintState == PaintState::abandoned) {
		// Painting area was insufficient to cover new styling or brace highlight positions
		FullPaint();
		::ValidateRect(MainHWND(), nullptr);
	}
	paintState = PaintState::notPainting;

	// Restore debug output state
	//Platform::ShowAssertionPopUps(assertsPopup);

	//Platform::DebugPrintf("Paint took %g\n", ep.Duration());
	return 0;
}

sptr_t ScintillaWin::HandleCompositionWindowed(uptr_t wParam, sptr_t lParam) {
	if (lParam & GCS_RESULTSTR) {
		if (const IMContext imc{ MainHWND() }) {
			AddWString(imc.GetCompositionString(GCS_RESULTSTR), CharacterSource::ImeResult);

			// Set new position after converted
			imc.SetCompositionWindow(PointMainCaret());
		}
		return 0;
	}
	return ::DefWindowProc(MainHWND(), WM_IME_COMPOSITION, wParam, lParam);
}

void ScintillaWin::SetCandidateWindowPos(const IMContext &imc) {
	imc.SetCandidateWindowPos(PointMainCaret(), inputLang, vs.lineHeight, sysCaretWidth, sysCaretHeight);
}

void ScintillaWin::SelectionToHangul() {
	// Convert every Hanja to Hangul within the main range.
	const Sci::Position selStart = sel.RangeMain().Start().Position();
	const Sci::Position documentStrLen = sel.RangeMain().Length();
	const Sci::Position selEnd = selStart + documentStrLen;
	const Sci::Position utf16Len = pdoc->CountUTF16(selStart, selEnd);

	if (utf16Len > 0) {
		const std::string documentStr = RangeText(selStart, selStart + documentStrLen);
		const UINT codePage = CodePageOfDocument();

		std::wstring uniStr = StringDecode(documentStr, codePage);
		const bool converted = HanjaDict::GetHangulOfHanja(uniStr);

		if (converted) {
			const std::string hangul = StringEncode(uniStr, CodePageOfDocument());
			const UndoGroup ug(pdoc);
			ClearSelection();
			InsertPaste(hangul.data(), hangul.size());
		}
	}
}

void ScintillaWin::EscapeHanja() {
	// The candidate box pops up to user to select a Hanja.
	// It comes into WM_IME_COMPOSITION with GCS_RESULTSTR.
	// The existing Hangul or Hanja is replaced with it.

	const Sci::Position currentPos = CurrentPosition();
	const int oneCharLen = pdoc->LenChar(currentPos);

	if (oneCharLen < 2) {
		return; // No need to handle SBCS.
	}

	const std::string oneChar = RangeText(currentPos, currentPos + oneCharLen);

	std::wstring uniChar = StringDecode(oneChar, CodePageOfDocument());
	// ImmEscapeW() may overwrite uniChar[] with a null terminated string.
	// So enlarge it enough to Maximum 4 as in UTF-8.
	uniChar.resize(UTF8MaxBytes);

	if (const IMContext imc{ MainHWND() }) {
		// Set the candidate box position since IME may show it.
		SetCandidateWindowPos(imc);
		// IME_ESC_HANJA_MODE appears to receive the first character only.
		if (imc.Escape(GetKeyboardLayout(0), IME_ESC_HANJA_MODE, uniChar.data())) {
			SetSelection(currentPos, currentPos + oneCharLen);
		}
	}
}

void ScintillaWin::ToggleHanja() {
	// If selection, convert every Hanja to Hangul within the main range.
	// If no selection, commit to IME.
	if (sel.Count() > 1) {
		return; // Do not allow multi carets.
	}

	if (sel.Empty()) {
		EscapeHanja();
	} else {
		SelectionToHangul();
	}
}

namespace {

// https://docs.microsoft.com/en-us/windows/desktop/Intl/composition-string
int MapImeIndicators(std::vector<BYTE> &inputStyle) noexcept {
	int mask = 0;
	static_assert(ATTR_INPUT < 4 && ATTR_TARGET_CONVERTED < 4 && ATTR_CONVERTED < 4 && ATTR_TARGET_NOTCONVERTED < 4);
	constexpr unsigned indicatorMask = (IndicatorInput << (8*ATTR_INPUT))
		| (IndicatorTarget << (8*ATTR_TARGET_CONVERTED))
		| (IndicatorConverted << (8*ATTR_CONVERTED))
		| (IndicatorTarget << (8*ATTR_TARGET_NOTCONVERTED));
	for (BYTE &style : inputStyle) {
		if (style > 3) {
			style = IndicatorUnknown;
			mask |= 1 << (IndicatorUnknown - IndicatorInput);
		} else {
			style = (indicatorMask >> (8*style)) & 0xff;
			mask |= 1 << (style - IndicatorInput);
		}
	}
	return mask;
}

}

void ScintillaWin::AddWString(std::wstring_view wsv, CharacterSource charSource) {
	if (wsv.empty())
		return;

	const UINT codePage = CodePageOfDocument();
	char inBufferCP[16];
	for (size_t i = 0; i < wsv.size(); ) {
		const size_t ucWidth = UTF16CharLength(wsv[i]);

		const int size = MultiByteFromWideChar(codePage, wsv.substr(i, ucWidth), inBufferCP, sizeof(inBufferCP) - 1);
		inBufferCP[size] = '\0';
		InsertCharacter(std::string_view(inBufferCP, size), charSource);
		i += ucWidth;
	}
}

bool ScintillaWin::HandleLaTeXTabCompletion() {
	if (ac.Active() || sel.Count() > 1 || !sel.Empty() || pdoc->IsReadOnly()) {
		return false;
	}

	const Sci::Position main = sel.MainCaret();
	if (main <= MinLaTeXInputSequenceLength) {
		return false;
	}

	char buffer[MaxLaTeXInputBufferLength];
#if 1
	Sci::Position pos = main - 1;
	char *ptr = buffer + sizeof(buffer) - 1;
	*ptr = '\0';
	char ch;
	do {
		ch = pdoc->CharAt(pos);
		if (!IsLaTeXInputSequenceChar(ch)) {
			break;
		}
		--pos;
		--ptr;
		*ptr = ch;
	} while (pos >= 0 && ptr != buffer);
	if (ch != '\\') {
		return false;
	}
	if (pdoc->dbcsCodePage && pdoc->dbcsCodePage != CpUtf8) {
		ch = pdoc->CharAt(pos - 1);
		if (!UTF8IsAscii(ch) && pdoc->IsDBCSLeadByteNoExcept(ch)) {
			return false;
		}
	}

	ptrdiff_t wclen = buffer + sizeof(buffer) - 1 - ptr;
#else
	Sci::Position pos = std::max<Sci::Position>(0, main - (sizeof(buffer) - 1));
	Sci::Position wclen = main - pos;
	pdoc->GetCharRange(buffer, pos, wclen);

	pos = main;
	char *ptr = buffer + wclen;
	*ptr = '\0';
	char ch;
	do {
		--pos;
		--ptr;
		ch = *ptr;
		if (!IsLaTeXInputSequenceChar(ch)) {
			break;
		}
	} while (ptr != buffer);
	if (ch != '\\') {
		return false;
	}
	if (pdoc->dbcsCodePage && pdoc->dbcsCodePage != CpUtf8) {
		ch = pdoc->CharAt(pos - 1);
		if (!UTF8IsAscii(ch) && pdoc->IsDBCSLeadByteNoExcept(ch)) {
			return false;
		}
	}

	++ptr;
	wclen = main - pos - 1;
#endif

	//const ElapsedPeriod elapsed;
	const uint32_t wch = GetLaTeXInputUnicodeCharacter(ptr, wclen);
	//const double duration = elapsed.Duration()*1e6;
	//printf("LaTeXInput(%s) => %04X, %.3f\n", ptr, wch, duration);
	if (wch == 0) {
		return false;
	}

	const wchar_t wcs[3] = { static_cast<wchar_t>(wch & 0xffff), static_cast<wchar_t>(wch >> 16), 0 };
	wclen = 1 + (wcs[1] != 0);

	const UINT codePage = CodePageOfDocument();
	const int len = MultiByteFromWideChar(codePage, std::wstring_view(wcs, wclen), buffer, sizeof(buffer) - 1);
	buffer[len] = '\0';

	targetRange.start.SetPosition(pos);
	targetRange.end.SetPosition(main);
	ReplaceTarget(Message::ReplaceTarget, len, AsInteger<sptr_t>(buffer));
	// move caret after character
	SetEmptySelection(pos + len);
	return true;
}

sptr_t ScintillaWin::HandleCompositionInline(uptr_t, sptr_t lParam) {
	// Copy & paste by johnsonj with a lot of helps of Neil.
	// Great thanks for my foreruners, jiniya and BLUEnLIVE.
	const IMContext imc(MainHWND());
	if (!imc) {
		return 0;
	}
	if (pdoc->IsReadOnly() || SelectionContainsProtected()) {
		imc.Notify(false);
		return 0;
	}

	const DelaySavePoint delay(pdoc);
	const bool tentative = pdoc->TentativeActive();
	if (tentative) {
		pdoc->TentativeUndo();
	}

	view.imeCaretBlockOverride = false;

	// See Chromium's InputMethodWinImm32::OnImeComposition()
	//
	// Japanese IMEs send a message containing both GCS_RESULTSTR and
	// GCS_COMPSTR, which means an ongoing composition has been finished
	// by the start of another composition.
	if (lParam & GCS_RESULTSTR) {
		AddWString(imc.GetCompositionString(GCS_RESULTSTR), CharacterSource::ImeResult);
	}

	if (lParam & GCS_COMPSTR) {
		const std::wstring wcs = imc.GetCompositionString(GCS_COMPSTR);
		// GCS_COMPSTR is set on pressing Esc, but without composition string.
		if (wcs.empty()) {
			ShowCaretAtCurrentPosition();
			return 0;
		}

		// No tentative undo means start of this composition so fill in any virtual spaces.
		if (!tentative) {
			ClearBeforeTentativeStart();
		}

		// Set candidate window left aligned to beginning of preedit string.
		SetCandidateWindowPos(imc);
		pdoc->TentativeStart(); // TentativeActive from now on.

		std::vector<BYTE> imeIndicator = imc.GetImeAttributes();
		const int indicatorMask = MapImeIndicators(imeIndicator);

		const UINT codePage = CodePageOfDocument();
		const std::wstring_view wsv = wcs;
		char inBufferCP[16];
		for (size_t i = 0; i < wsv.size(); ) {
			const size_t ucWidth = UTF16CharLength(wsv[i]);
			const int size = MultiByteFromWideChar(codePage, wsv.substr(i, ucWidth), inBufferCP, sizeof(inBufferCP) - 1);
			inBufferCP[size] = '\0';
			InsertCharacter(std::string_view(inBufferCP, size), CharacterSource::TentativeInput);

			DrawImeIndicator(imeIndicator[i], size);
			i += ucWidth;
		}

		// Japanese IME after pressing Space or Tab replaces input string with first candidate item (target string);
		// when selecting other candidate item, previous item will be replaced with current one.
		// After candidate item been added, it's looks like been full selected, it's better to keep caret
		// at end of "selection" (end of input) instead of jump to beginning of input ("selection").
		constexpr int targetMask = 1 << (IndicatorTarget - IndicatorInput);
		if (indicatorMask != targetMask) {
			// From Chromium: Retrieve the selection range information. If CS_NOMOVECARET is specified,
			// that means the cursor should not be moved, then we just place the caret at
			// the beginning of the composition string. Otherwise we should honour the
			// GCS_CURSORPOS value if it's available.
			Sci::Position imeEndToImeCaretU16 = -static_cast<Sci::Position>(wcs.size());
			if (!(lParam & CS_NOMOVECARET) && (lParam & GCS_CURSORPOS)) {
				imeEndToImeCaretU16 += imc.GetImeCaretPos();
			}
			if (imeEndToImeCaretU16 != 0) {
				// Move back IME caret from current last position to imeCaretPos.
				const Sci::Position currentPos = CurrentPosition();
				const Sci::Position imeCaretPosDoc = pdoc->GetRelativePositionUTF16(currentPos, imeEndToImeCaretU16);
				MoveImeCarets(-currentPos + imeCaretPosDoc);
				if (indicatorMask & targetMask) {
					// set candidate window left aligned to beginning of target string.
					SetCandidateWindowPos(imc);
				}
			}
		}

		view.imeCaretBlockOverride = KoreanIME();
		HideCursorIfPreferred();
	}

	EnsureCaretVisible();
	ShowCaretAtCurrentPosition();
	return 0;
}

namespace {

// Translate message IDs from WM_* and EM_* to SCI_* so can partly emulate Windows Edit control
constexpr Message SciMessageFromEM(unsigned int iMessage) noexcept {
	switch (iMessage) {
	case EM_CANPASTE: return Message::CanPaste;
	case EM_CANREDO: return Message::CanRedo;
	case EM_CANUNDO: return Message::CanUndo;
	case EM_EMPTYUNDOBUFFER: return Message::EmptyUndoBuffer;
	case EM_GETFIRSTVISIBLELINE: return Message::GetFirstVisibleLine;
	case EM_GETLINE: return Message::GetLine;
	case EM_GETLINECOUNT: return Message::GetLineCount;
	case EM_GETSELTEXT: return Message::GetSelText;
	case EM_HIDESELECTION: return Message::HideSelection;
	case EM_LINEINDEX: return Message::PositionFromLine;
	case EM_LINESCROLL: return Message::LineScroll;
	case EM_REDO: return Message::Redo;
	case EM_REPLACESEL: return Message::ReplaceSel;
	case EM_SCROLLCARET: return Message::ScrollCaret;
	case EM_SETREADONLY: return Message::SetReadOnly;
	case EM_UNDO: return Message::Undo;
	case WM_CLEAR: return Message::Clear;
	case WM_COPY: return Message::Copy;
	case WM_CUT: return Message::Cut;
	case WM_PASTE: return Message::Paste;
	case WM_SETTEXT: return Message::SetText;
	case WM_UNDO: return Message::Undo;
	default: return static_cast<Message>(iMessage);
	}
}

}

UINT ScintillaWin::CodePageOfDocument() const noexcept {
	return pdoc->dbcsCodePage; // see Message::GetCodePage in Editor.cxx
}

std::string ScintillaWin::EncodeWString(std::wstring_view wsv) const {
	if (IsUnicodeMode()) {
		const size_t len = UTF8Length(wsv);
		std::string putf(len, '\0');
		UTF8FromUTF16(wsv, putf.data(), len);
		return putf;
	}
	// Not in Unicode mode so convert from Unicode to current Scintilla code page
	return StringEncode(wsv, CodePageOfDocument());
}

sptr_t ScintillaWin::GetTextLength() const noexcept {
	return pdoc->CountUTF16(0, pdoc->LengthNoExcept());
	// Count the number of UTF-16 code units line by line
}

sptr_t ScintillaWin::GetText(uptr_t wParam, sptr_t lParam) const {
	if (lParam == 0) {
		return pdoc->CountUTF16(0, pdoc->LengthNoExcept());
	}
	if (wParam == 0) {
		return 0;
	}
	wchar_t *ptr = AsPointer<wchar_t *>(lParam);
	if (pdoc->LengthNoExcept() == 0) {
		*ptr = L'\0';
		return 0;
	}
	const Sci::Position lengthWanted = wParam - 1;
	Sci::Position sizeRequestedRange = pdoc->GetRelativePositionUTF16(0, lengthWanted);
	if (sizeRequestedRange < 0) {
		// Requested more text than there is in the document.
		sizeRequestedRange = pdoc->LengthNoExcept();
	}
	std::string docBytes(sizeRequestedRange, '\0');
	pdoc->GetCharRange(docBytes.data(), 0, sizeRequestedRange);
	// Convert to Unicode using the current Scintilla code page
	const UINT cpSrc = CodePageOfDocument();
	int lengthUTF16 = WideCharLenFromMultiByte(cpSrc, docBytes);
	if (lengthUTF16 > lengthWanted)
		lengthUTF16 = static_cast<int>(lengthWanted);
	WideCharFromMultiByte(cpSrc, docBytes, ptr, lengthUTF16);
	ptr[lengthUTF16] = L'\0';
	return lengthUTF16;
	// Not Unicode mode
	// Convert to Unicode using the current Scintilla code page
	// Retrieve as UTF-16 line by line
}

Window::Cursor ScintillaWin::ContextCursor(Point pt) {
	if (inDragDrop == DragDrop::dragging) {
		return Window::Cursor::up;
	}
	// Display regular (drag) cursor over selection
	if (PointInSelMargin(pt)) {
		return GetMarginCursor(pt);
	} else if (!SelectionEmpty() && PointInSelection(pt)) {
		return Window::Cursor::arrow;
	} else if (PointIsHotspot(pt)) {
		return Window::Cursor::hand;
	} else if (hoverIndicatorPos != Sci::invalidPosition) {
		const Sci::Position pos = PositionFromLocation(pt, true, true);
		if (pos != Sci::invalidPosition) {
			return Window::Cursor::hand;
		}
	}
	return Window::Cursor::text;
}

#if SCI_EnablePopupMenu
sptr_t ScintillaWin::ShowContextMenu(unsigned int iMessage, uptr_t wParam, sptr_t lParam) {
	Point ptScreen = PointFromLParam(lParam);
	Point ptClient;
	POINT point = POINTFromLParam(lParam);
	if ((point.x == -1) && (point.y == -1)) {
		// Caused by keyboard so display menu near caret
		ptClient = PointMainCaret();
		point = POINTFromPoint(ptClient);
		::ClientToScreen(MainHWND(), &point);
		ptScreen = PointFromPOINT(point);
	} else {
		::ScreenToClient(MainHWND(), &point);
		ptClient = PointFromPOINT(point);
	}
	if (ShouldDisplayPopup(ptClient)) {
		ContextMenu(ptScreen);
		return 0;
	}
	return ::DefWindowProc(MainHWND(), iMessage, wParam, lParam);
}
#endif

void ScintillaWin::SizeWindow() {
	rectangleClient = wMain.GetClientPosition();
	HRESULT hrResize = E_FAIL;
	if (((technology == Technology::DirectWrite) || (technology == Technology::DirectWriteRetain)) && targets.pHwndRT) {
		// May be able to just resize the HWND render target
		const D2D1_SIZE_U pixelSize = GetSizeUFromRect(GetClientRect(MainHWND()));
		hrResize = targets.pHwndRT->Resize(pixelSize);
		if (FAILED(hrResize)) {
			// Platform::DebugPrintf("Failed to Resize ID2D1HwndRenderTarget 0x%lx\n", hrResize);
		}
	}
#if _WIN32_WINNT >= _WIN32_WINNT_WIN7
	if ((technology == Technology::DirectWrite1) && pDXGISwapChain && targets.pDeviceContext &&
		(paintState == PaintState::notPainting)) {
		targets.pDeviceContext->SetTarget(nullptr);	// ResizeBuffers fails if bitmap still owned by swap chain
		hrResize = pDXGISwapChain->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
		if (SUCCEEDED(hrResize)) {
			hrResize = SetBackBuffer(pDXGISwapChain.Get());
		} else {
			// Platform::DebugPrintf("Failed ResizeBuffers 0x%lx\n", hrResize);
		}
	}
#endif
	if (FAILED(hrResize)) {
		if (paintState == PaintState::notPainting) {
			DropRenderTarget();
		} else {
			targets.valid = false;
		}
	}
	//Platform::DebugPrintf("Scintilla WM_SIZE %d %d\n", LOWORD(lParam), HIWORD(lParam));
	ChangeSize();
}

sptr_t ScintillaWin::MouseMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam) {
	switch (iMessage) {
	case WM_LBUTTONDOWN:
		// For IME, set the composition string as the result string.
		if (const IMContext imc{ MainHWND() }) {
			imc.Notify(true);
		}
		::SetFocus(MainHWND());
		ButtonDownWithModifiers(PointFromLParam(lParam), ::GetMessageTime(),
					MouseModifiers(wParam));
	break;

	case WM_LBUTTONUP:
		ButtonUpWithModifiers(PointFromLParam(lParam),
						::GetMessageTime(), MouseModifiers(wParam));
		break;

	case WM_RBUTTONDOWN: {
		::SetFocus(MainHWND());
		const Point pt = PointFromLParam(lParam);
		if (!PointInSelection(pt)) {
			CancelModes();
			SetEmptySelection(PositionFromLocation(PointFromLParam(lParam)));
		}

		RightButtonDownWithModifiers(pt, ::GetMessageTime(), MouseModifiers(wParam));
	}
	break;

	case WM_MOUSEMOVE: {
		cursorIsHidden = false; // to be shown by ButtonMoveWithModifiers
		const Point pt = PointFromLParam(lParam);

		// Windows might send WM_MOUSEMOVE even though the mouse has not been moved:
		// http://blogs.msdn.com/b/oldnewthing/archive/2003/10/01/55108.aspx
		if (ptMouseLast != pt) {
			SetTrackMouseLeaveEvent(true);
			ButtonMoveWithModifiers(pt, ::GetMessageTime(), MouseModifiers(wParam));
		}
	}
	break;

	case WM_MOUSELEAVE:
		SetTrackMouseLeaveEvent(false);
		MouseLeave();
		return ::DefWindowProc(MainHWND(), iMessage, wParam, lParam);

	case WM_MOUSEWHEEL:
	case WM_MOUSEHWHEEL:
		if (!mouseWheelCaptures) {
			// if the mouse wheel is not captured, test if the mouse
			// pointer is over the editor window and if not, don't
			// handle the message but pass it on.
			RECT rc;
			GetWindowRect(MainHWND(), &rc);
			const POINT pt = POINTFromLParam(lParam);
			if (!PtInRect(&rc, pt)) {
				return ::DefWindowProc(MainHWND(), iMessage, wParam, lParam);
			}
		}

		// if autocomplete list active then send mousewheel message to it
		if (ac.Active()) {
			HWND hWnd = HwndFromWindow(*(ac.lb));
			::SendMessage(hWnd, iMessage, wParam, lParam);
			break;
		}

		// Treat Shift+WM_MOUSEWHEEL as horizontal scrolling, not data-zoom.
		if (iMessage == WM_MOUSEHWHEEL || (wParam & MK_SHIFT)) {
			if (vs.wrap.state != Wrap::None || charsPerScroll == 0) {
				return ::DefWindowProc(MainHWND(), iMessage, wParam, lParam);
			}

			MouseWheelDelta &wheelDelta = (iMessage == WM_MOUSEHWHEEL) ? horizontalWheelDelta : verticalWheelDelta;
			if (wheelDelta.Accumulate(wParam)) {
				int charsToScroll = charsPerScroll * wheelDelta.Actions();
				if (iMessage == WM_MOUSEHWHEEL) {
					// horizontal scroll is in reverse direction
					charsToScroll = -charsToScroll;
				}
				const int widthToScroll = static_cast<int>(std::lround(charsToScroll * vs.aveCharWidth));
				HorizontalScrollToClamped(xOffset + widthToScroll);
			}
			// return 1 for Logitech mouse, https://www.pretentiousname.com/setpoint_hwheel/index.html
			return (iMessage == WM_MOUSEHWHEEL) ? 1 : 0;
		}

		// Either SCROLL or ZOOM. We handle the wheel steppings calculation
		if (linesPerScroll != 0 && verticalWheelDelta.Accumulate(wParam)) {
			Sci::Line linesToScroll = linesPerScroll;
			if (linesPerScroll == WHEEL_PAGESCROLL) {
				linesToScroll = LinesOnScreen() - 1;
			}
			linesToScroll = std::max<Sci::Line>(linesToScroll, 1);
			linesToScroll *= verticalWheelDelta.Actions();

			if (wParam & MK_CONTROL) {
				// Zoom! We play with the font sizes in the styles.
				// Number of steps/line is ignored, we just care if sizing up or down
				if (linesToScroll < 0) {
					KeyCommand(Message::ZoomIn);
				} else {
					KeyCommand(Message::ZoomOut);
				}
			} else {
				// Scroll
				ScrollTo(topLine + linesToScroll);
			}
		}
		return 0;
	}
	return 0;
}

sptr_t ScintillaWin::KeyMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam) {
	switch (iMessage) {

	case WM_SYSKEYDOWN:
	case WM_KEYDOWN: {
		// Platform::DebugPrintf("Keydown %c %c%c%c%c %x %x\n",
		// iMessage == WM_KEYDOWN ? 'K' : 'S',
		// (lParam & (1 << 24)) ? 'E' : '-',
		// KeyboardIsKeyDown(VK_SHIFT) ? 'S' : '-',
		// KeyboardIsKeyDown(VK_CONTROL) ? 'C' : '-',
		// KeyboardIsKeyDown(VK_MENU) ? 'A' : '-',
		// wParam, lParam);
		lastKeyDownConsumed = false;
		const bool altDown = KeyboardIsKeyDown(VK_MENU);
		if (altDown && KeyboardIsNumericKeypadFunction(wParam, lParam)) {
			// Don't interpret these as they may be characters entered by number.
			return ::DefWindowProc(MainHWND(), iMessage, wParam, lParam);
		}
		const KeyMod modifiers = ModifierFlags(KeyboardIsKeyDown(VK_SHIFT), KeyboardIsKeyDown(VK_CONTROL), altDown);
		const int ret = KeyDownWithModifiers(KeyTranslate(wParam), modifiers, &lastKeyDownConsumed);
		if (!ret && !lastKeyDownConsumed) {
#if 0
			if (FlagSet(modifiers, KeyMod::Ctrl)) {
				if (::SendMessage(::GetParent(MainHWND()), WM_KEYDOWN, wParam, lParam)) {
					lastKeyDownConsumed = true;
					break;
				}
			}
#endif
			return ::DefWindowProc(MainHWND(), iMessage, wParam, lParam);
		}
		break;
	}

	case WM_KEYUP:
		//Platform::DebugPrintf("S keyup %d %x %x\n",iMessage, wParam, lParam);
		return ::DefWindowProc(MainHWND(), iMessage, wParam, lParam);

	case WM_CHAR:
		//printf("%s:%d WM_CHAR %u, consumed=%d\n", __func__, __LINE__, (UINT)wParam, lastKeyDownConsumed);
		HideCursorIfPreferred();
		if (wParam >= ' ' || !lastKeyDownConsumed) {
			// filter out control characters
			// https://docs.microsoft.com/en-us/windows/win32/learnwin32/keyboard-input#character-messages
			// What's broken in the WM_KEYDOWN-WM_CHAR input model?
			// http://www.ngedit.com/a_wm_keydown_wm_char.html
			if (wParam < ' ' && KeyboardIsKeyDown(VK_CONTROL)) {
				if (::SendMessage(::GetParent(MainHWND()), WM_CHAR, wParam, lParam)) {
					return 0;
				}
			}

			const wchar_t charCode = static_cast<wchar_t>(wParam);
			if (IS_HIGH_SURROGATE(charCode)) {
				// If this is a high surrogate character, we need a second one
				lastHighSurrogateChar = charCode;
			} else {
				wchar_t wcs[3] = { charCode, 0 };
				unsigned int wclen = 1;
				if (IS_LOW_SURROGATE(charCode)) {
					wcs[1] = charCode;
					wcs[0] = lastHighSurrogateChar;
					lastHighSurrogateChar = 0;
					wclen = 2;
				}
				AddWString(std::wstring_view(wcs, wclen), CharacterSource::DirectInput);
			}
		}
		return 0;

	case WM_UNICHAR:
		if (wParam == UNICODE_NOCHAR) {
			return TRUE;
		} else if (lastKeyDownConsumed) {
			return 1;
		} else {
			wchar_t wcs[3]{};
			const size_t wclen = UTF16FromUTF32Character(static_cast<unsigned int>(wParam), wcs);
			AddWString(std::wstring_view(wcs, wclen), CharacterSource::DirectInput);
			return FALSE;
		}
	}

	return 0;
}

sptr_t ScintillaWin::FocusMessage(unsigned int iMessage, uptr_t wParam, sptr_t) {
	switch (iMessage) {
	case WM_KILLFOCUS: {
		HWND wOther = AsPointer<HWND>(wParam);
		HWND wThis = MainHWND();
		HWND wCT = HwndFromWindow(ct.wCallTip);
		if (!wParam ||
			!(::IsChild(wThis, wOther) || (wOther == wCT))) {
			SetFocusState(false);
			DestroySystemCaret();
		}
		// Explicitly complete any IME composition
		if (const IMContext imc{ MainHWND() }) {
			imc.Notify(true);
		}
		break;
	}

	case WM_SETFOCUS:
		SetFocusState(true);
		DestroySystemCaret();
		CreateSystemCaret();
		break;
	}
	return 0;
}

sptr_t ScintillaWin::IMEMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam) {
	switch (iMessage) {

	case WM_INPUTLANGCHANGE:
		inputLang = InputLanguage();
		return ::DefWindowProc(MainHWND(), iMessage, wParam, lParam);

	case WM_INPUTLANGCHANGEREQUEST:
		return ::DefWindowProc(MainHWND(), iMessage, wParam, lParam);

	case WM_IME_KEYDOWN: {
		if (wParam == VK_HANJA) {
			// On US keyboards with Korean Microsoft IME, VK_HANJA is right Ctrl
			ToggleHanja();
		}
		return ::DefWindowProc(MainHWND(), iMessage, wParam, lParam);
	}

	case WM_IME_REQUEST: {
		if (wParam == IMR_RECONVERTSTRING) {
			return ImeOnReconvert(lParam);
		}
		if (wParam == IMR_DOCUMENTFEED) {
			return ImeOnDocumentFeed(lParam);
		}
		return ::DefWindowProc(MainHWND(), iMessage, wParam, lParam);
	}

	case WM_IME_STARTCOMPOSITION:
		if (KoreanIME() || imeInteraction == IMEInteraction::Inline) {
			return 0;
		} else {
			ImeStartComposition();
			return ::DefWindowProc(MainHWND(), iMessage, wParam, lParam);
		}

	case WM_IME_ENDCOMPOSITION:
		ImeEndComposition();
		return ::DefWindowProc(MainHWND(), iMessage, wParam, lParam);

	case WM_IME_COMPOSITION:
		if (KoreanIME() || imeInteraction == IMEInteraction::Inline) {
			return HandleCompositionInline(wParam, lParam);
		} else {
			return HandleCompositionWindowed(wParam, lParam);
		}

	case WM_IME_SETCONTEXT:
		if (wParam) { // window is activated
			inputLang = InputLanguage();

			if (KoreanIME() || imeInteraction == IMEInteraction::Inline) {
				// hide IME's composition window.
				lParam &= ~ISC_SHOWUICOMPOSITIONWINDOW;
			}
		}
		return ::DefWindowProc(MainHWND(), iMessage, wParam, lParam);

	case WM_IME_NOTIFY:
		return ::DefWindowProc(MainHWND(), iMessage, wParam, lParam);

	}
	return 0;
}

sptr_t ScintillaWin::EditMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam) {
	switch (iMessage) {

	case EM_FINDTEXT:
		if (lParam == 0) {
			return -1;
		} else {
			const FINDTEXTA *pFT = AsPointer<const FINDTEXTA *>(lParam);
			TextToFindFull tt = { { pFT->chrg.cpMin, pFT->chrg.cpMax }, pFT->lpstrText, {} };
			return ScintillaBase::WndProc(Message::FindTextFull, wParam, AsInteger<sptr_t>(&tt));
		}

	case EM_FINDTEXTEX:
		if (lParam == 0) {
			return -1;
		} else {
			FINDTEXTEXA *pFT = AsPointer<FINDTEXTEXA *>(lParam);
			TextToFindFull tt = { { pFT->chrg.cpMin, pFT->chrg.cpMax }, pFT->lpstrText, {} };
			const Sci::Position pos =ScintillaBase::WndProc(Message::FindTextFull, wParam, AsInteger<sptr_t>(&tt));
			pFT->chrgText.cpMin = (pos < 0)? -1 : static_cast<LONG>(tt.chrgText.cpMin);
			pFT->chrgText.cpMax = (pos < 0)? -1 : static_cast<LONG>(tt.chrgText.cpMax);
			return pos;
		}

	case EM_FORMATRANGE:
		if (lParam) {
			const FORMATRANGE *pFR = AsPointer<const FORMATRANGE *>(lParam);
			const RangeToFormatFull fr = { pFR->hdcTarget, pFR->hdc,
				{ pFR->rc.left, pFR->rc.top, pFR->rc.right, pFR->rc.bottom },
				{ pFR->rcPage.left, pFR->rcPage.top, pFR->rcPage.right, pFR->rcPage.bottom },
				{ pFR->chrg.cpMin, pFR->chrg.cpMax },
			};
			return ScintillaBase::WndProc(Message::FormatRangeFull, wParam, AsInteger<sptr_t>(&fr));
		}
		break;

	case EM_GETTEXTRANGE:
		if (lParam) {
			TEXTRANGEA *pTR = AsPointer<TEXTRANGEA *>(lParam);
			TextRangeFull tr = { { pTR->chrg.cpMin, pTR->chrg.cpMax }, pTR->lpstrText };
			return ScintillaBase::WndProc(Message::GetTextRangeFull, 0, AsInteger<sptr_t>(&tr));
		}
		break;

	case EM_LINEFROMCHAR:
		if (PositionFromUPtr(wParam) < 0) {
			wParam = SelectionStart().Position();
		}
		return pdoc->SciLineFromPosition(wParam);

	case EM_EXLINEFROMCHAR:
		return pdoc->SciLineFromPosition(lParam);

	case EM_GETSEL:
		if (wParam) {
			*AsPointer<DWORD *>(wParam) = static_cast<DWORD>(SelectionStart().Position());
		}
		if (lParam) {
			*AsPointer<DWORD *>(lParam) = static_cast<DWORD>(SelectionEnd().Position());
		}
		return MAKELRESULT(SelectionStart().Position(), SelectionEnd().Position());

	case EM_EXGETSEL: {
		if (lParam == 0) {
			return 0;
		}
		CHARRANGE *pCR = AsPointer<CHARRANGE *>(lParam);
		pCR->cpMin = static_cast<LONG>(SelectionStart().Position());
		pCR->cpMax = static_cast<LONG>(SelectionEnd().Position());
	}
	break;

	case EM_SETSEL: {
		Sci::Position nStart = wParam;
		Sci::Position nEnd = lParam;
		if (nStart == 0 && nEnd < 0) {
			nEnd = pdoc->LengthNoExcept();
		}
		if (nStart < 0) {
			nStart = nEnd;	// Remove selection
		}
		SetSelection(nEnd, nStart);
		EnsureCaretVisible();
	}
	break;

	case EM_EXSETSEL: {
		if (lParam == 0) {
			return 0;
		}
		const CHARRANGE *pCR = AsPointer<const CHARRANGE *>(lParam);
		const Sci::Position cpMax = (pCR->cpMax < 0) ? pdoc->LengthNoExcept() : pCR->cpMax;
		sel.selType = Selection::SelTypes::stream;
		SetSelection(pCR->cpMin, cpMax);
		EnsureCaretVisible();
		return pdoc->SciLineFromPosition(SelectionStart().Position());
	}

	case EM_LINELENGTH:
		return ScintillaBase::WndProc(Message::LineLength, pdoc->SciLineFromPosition(wParam), lParam);

	case EM_POSFROMCHAR:
		if (wParam) {
			const Point pt = LocationFromPosition(lParam);
			POINTL *ptw = AsPointer<POINTL *>(wParam);
			ptw->x = static_cast<LONG>(pt.x - vs.textStart + vs.fixedColumnWidth); // SCI_POINTXFROMPOSITION
			ptw->y = static_cast<LONG>(pt.y);
		}
		break;

	case EM_GETZOOM:
		if (wParam && lParam) {
			*AsPointer<int *>(wParam) = 16*vs.zoomLevel/25;
			*AsPointer<int *>(lParam) = 64;
			return TRUE;
		}
		break;

	case EM_SETZOOM: {
		int level = 0;
		if (wParam == 0 && lParam == 0) {
			level = 100;
		} else if (wParam != 0 && lParam > 0) {
			level = static_cast<int>(wParam/lParam);
		}
		if (level != 0) {
			ScintillaBase::WndProc(Message::SetZoom, level, 0);
			return TRUE;
		}
	}
	break;

	}
	return 0;
}

sptr_t ScintillaWin::IdleMessage(unsigned int iMessage, uptr_t wParam, sptr_t lParam) {
	switch (iMessage) {
	case SC_WIN_IDLE:
		// wParam=dwTickCountInitial, or 0 to initialize.  lParam=bSkipUserInputTest
		if (idler.state) {
			if (lParam || (WAIT_TIMEOUT == MsgWaitForMultipleObjects(0, nullptr, 0, 0, QS_INPUT | QS_HOTKEY))) {
				if (Idle()) {
					// User input was given priority above, but all events do get a turn.  Other
					// messages, notifications, etc. will get interleaved with the idle messages.

					// However, some things like WM_PAINT are a lower priority, and will not fire
					// when there's a message posted.  So, several times a second, we stop and let
					// the low priority events have a turn (after which the timer will fire again).

					// Suppress a warning from Code Analysis that the GetTickCount function
					// wraps after 49 days. The WM_TIMER will kick off another SC_WIN_IDLE
					// after the wrap.

					const DWORD dwCurrent = GetTickCount();
					const DWORD dwStart = wParam ? static_cast<DWORD>(wParam) : dwCurrent;
					constexpr DWORD maxWorkTime = 50;

					if (dwCurrent >= dwStart && dwCurrent > maxWorkTime &&dwCurrent - maxWorkTime < dwStart) {
						PostMessage(MainHWND(), SC_WIN_IDLE, dwStart, 0);
					}
				} else {
					SetIdle(false);
				}
			}
		}
		break;

	case SC_WORK_IDLE:
		IdleWork();
		break;
	}
	return 0;
}

sptr_t ScintillaWin::SciMessage(Message iMessage, uptr_t wParam, sptr_t lParam) {
	switch (iMessage) {
	case Message::GetDirectFunction:
		//return AsInteger<sptr_t>(DirectFunction);
		return 0;

	case Message::GetDirectStatusFunction:
		//return AsInteger<sptr_t>(DirectStatusFunction);
		return 0;

	case Message::GetDirectPointer:
		return AsInteger<sptr_t>(this);

	case Message::GrabFocus:
		::SetFocus(MainHWND());
		break;

	case Message::SetTechnology:
		if (const Technology technologyNew = static_cast<Technology>(wParam);
			(technologyNew == Technology::Default) ||
			(technologyNew == Technology::DirectWriteRetain) ||
			(technologyNew == Technology::DirectWriteDC) ||
			(technologyNew == Technology::DirectWrite) ||
			(technologyNew == Technology::DirectWrite1)) {
			if (technology != technologyNew) {
				if (technologyNew != Technology::Default) {
					if (!LoadD2D()) {
						// Failed to load Direct2D or DirectWrite so no effect
						return 0;
					}
				} else {
					bidirectional = Bidirectional::Disabled;
				}
				DropRenderTarget();
				view.bufferedDraw = technologyNew == Technology::Default;
				technology = technologyNew;
				// Invalidate all cached information including layout.
				vs.fontsValid = false;
				UpdateRenderingParams(true);
				InvalidateStyleRedraw();
			}
		}
		break;

	case Message::SetBidirectional:
		if (technology == Technology::Default) {
			bidirectional = Bidirectional::Disabled;
		} else if (static_cast<Bidirectional>(wParam) <= Bidirectional::R2L) {
			bidirectional = static_cast<Bidirectional>(wParam);
		}
		// Invalidate all cached information including layout.
		InvalidateStyleRedraw();
		break;

	case Message::TargetAsUTF8:
		return TargetAsUTF8(CharPtrFromSPtr(lParam));

	case Message::EncodedFromUTF8:
		return EncodedFromUTF8(ConstCharPtrFromUPtr(wParam),
			CharPtrFromSPtr(lParam));

	default:
		break;
	}
	return 0;
}

sptr_t ScintillaWin::WndProc(Message iMessage, uptr_t wParam, sptr_t lParam) {
	try {
		//Platform::DebugPrintf("S M:%x WP:%x L:%x\n", iMessage, wParam, lParam);
		const unsigned int msg = static_cast<unsigned int>(iMessage);
		switch (msg) {

		case WM_CREATE:
			ctrlID = ::GetDlgCtrlID(MainHWND());
			UpdateBaseElements();
			GetMouseParameters();
			::RegisterDragDrop(MainHWND(), &dt);
			break;

		case WM_COMMAND:
#if SCI_EnablePopupMenu
			Command(LOWORD(wParam));
#endif
			break;

		case WM_PAINT:
			return WndPaint();

		case WM_PRINTCLIENT: {
			HDC hdc = AsPointer<HDC>(wParam);
			if (!IsCompatibleDC(hdc)) {
				return ::DefWindowProc(MainHWND(), msg, wParam, lParam);
			}
			FullPaintDC(hdc);
		}
		break;

		case WM_VSCROLL:
		case EM_SCROLL:
			ScrollMessage(wParam);
			break;

		case WM_HSCROLL:
			HorizontalScrollMessage(wParam);
			break;

		case WM_SIZE:
			SizeWindow();
			break;

		case WM_TIMER:
			if (wParam == idleTimerID && idler.state) {
				::SendMessage(MainHWND(), SC_WIN_IDLE, 0, 1);
			} else {
				TickFor(static_cast<TickReason>(wParam - fineTimerStart));
			}
			break;

		case SC_WIN_IDLE:
		case SC_WORK_IDLE:
			return IdleMessage(msg, wParam, lParam);

		case WM_GETMINMAXINFO:
			return ::DefWindowProc(MainHWND(), msg, wParam, lParam);

		case WM_LBUTTONDOWN:
		case WM_LBUTTONUP:
		case WM_RBUTTONDOWN:
		case WM_MOUSEMOVE:
		case WM_MOUSELEAVE:
		case WM_MOUSEWHEEL:
		case WM_MOUSEHWHEEL:
			return MouseMessage(msg, wParam, lParam);

		case WM_SETCURSOR:
			if (LOWORD(lParam) == HTCLIENT) {
				if (!cursorIsHidden) {
					POINT pt;
					if (::GetCursorPos(&pt)) {
						::ScreenToClient(MainHWND(), &pt);
						DisplayCursor(ContextCursor(PointFromPOINTEx(pt)));
					}
				}
				return TRUE;
			}
			return ::DefWindowProc(MainHWND(), msg, wParam, lParam);

		case WM_SYSKEYDOWN:
		case WM_KEYDOWN:
		case WM_KEYUP:
		case WM_CHAR:
		case WM_UNICHAR:
			return KeyMessage(msg, wParam, lParam);

		case WM_SETTINGCHANGE:
			//printf("%s before %s\n", GetCurrentLogTime(), "WM_SETTINGCHANGE");
			//Platform::DebugPrintf("Setting Changed\n");
			UpdateRenderingParams(true);
			UpdateBaseElements();
			GetMouseParameters();
			InvalidateStyleRedraw();
			//printf("%s after %s\n", GetCurrentLogTime(), "WM_SETTINGCHANGE");
			break;

		case WM_GETDLGCODE:
			return DLGC_HASSETSEL | DLGC_WANTALLKEYS;

		case WM_KILLFOCUS:
		case WM_SETFOCUS:
			return FocusMessage(msg, wParam, lParam);

		case WM_SYSCOLORCHANGE:
			//Platform::DebugPrintf("Setting Changed\n");
			//printf("%s before %s\n", GetCurrentLogTime(), "WM_SYSCOLORCHANGE");
			UpdateBaseElements();
			InvalidateStyleData();
			//printf("%s after %s\n", GetCurrentLogTime(), "WM_SYSCOLORCHANGE");
			break;

		case WM_DPICHANGED:
			dpi = HIWORD(wParam);
			reverseArrowCursor.Invalidate();
			vs.fontsValid = false;
			InvalidateStyleRedraw();
			break;

		case WM_DPICHANGED_AFTERPARENT: {
			const UINT dpiNow = GetWindowDPI(MainHWND());
			if (dpi != dpiNow) {
				dpi = dpiNow;
				reverseArrowCursor.Invalidate();
				vs.fontsValid = false;
				InvalidateStyleRedraw();
			}
		}
		break;

		case WM_CONTEXTMENU:
#if SCI_EnablePopupMenu
			return ShowContextMenu(msg, wParam, lParam);
#else
			return ::DefWindowProc(MainHWND(), msg, wParam, lParam);
#endif

		case WM_ERASEBKGND:
			return 1;   // Avoid any background erasure as whole window painted.

		case WM_SETREDRAW:
			::DefWindowProc(MainHWND(), msg, wParam, lParam);
			if (wParam) {
				SetIdleTaskTime(IdleLineWrapTime);
				SetScrollBars();
				SetVerticalScrollPos();
				SetHorizontalScrollPos();
			}
			return 0;

		case WM_CAPTURECHANGED:
			capturedMouse = false;
			return 0;

			// These are not handled in Scintilla and it's faster to dispatch them here.
			// Also moves time out to here so profile doesn't count lots of empty message calls.

		case WM_MOVE:
		case WM_MOUSEACTIVATE:
		case WM_NCHITTEST:
		case WM_NCCALCSIZE:
		case WM_NCPAINT:
		case WM_NCMOUSEMOVE:
		case WM_NCLBUTTONDOWN:
		case WM_SYSCOMMAND:
		case WM_WINDOWPOSCHANGING:
		case WM_WINDOWPOSCHANGED:
			return ::DefWindowProc(MainHWND(), msg, wParam, lParam);

#if 0 // we don't use Scintilla as top level window
		case WM_WINDOWPOSCHANGED:
			if (UpdateRenderingParams(false)) {
				DropGraphics();
				Redraw();
			}
			return ::DefWindowProc(MainHWND(), msg, wParam, lParam);
#endif

		case WM_GETTEXTLENGTH:
			return GetTextLength();

		case WM_GETTEXT:
			return GetText(wParam, lParam);

		case WM_INPUTLANGCHANGE:
		case WM_INPUTLANGCHANGEREQUEST:
		case WM_IME_KEYDOWN:
		case WM_IME_REQUEST:
		case WM_IME_STARTCOMPOSITION:
		case WM_IME_ENDCOMPOSITION:
		case WM_IME_COMPOSITION:
		case WM_IME_SETCONTEXT:
		case WM_IME_NOTIFY:
			return IMEMessage(msg, wParam, lParam);

		case EM_LINEFROMCHAR:
		case EM_EXLINEFROMCHAR:
		case EM_FINDTEXT:
		case EM_FINDTEXTEX:
		case EM_FORMATRANGE:
		case EM_GETTEXTRANGE:
		case EM_GETSEL:
		case EM_EXGETSEL:
		case EM_SETSEL:
		case EM_EXSETSEL:
		case EM_LINELENGTH:
		case EM_POSFROMCHAR:
		case EM_GETZOOM:
		case EM_SETZOOM:
			return EditMessage(msg, wParam, lParam);
		}

		iMessage = SciMessageFromEM(msg);
		switch (iMessage) {
		case Message::GetDirectFunction:
		case Message::GetDirectStatusFunction:
		case Message::GetDirectPointer:
		case Message::GrabFocus:
		case Message::SetTechnology:
		case Message::SetBidirectional:
		case Message::TargetAsUTF8:
		case Message::EncodedFromUTF8:
			return SciMessage(iMessage, wParam, lParam);

		case Message::Tab:
			if (wParam & static_cast<int>(TabCompletion::Latex)) {
				if (HandleLaTeXTabCompletion()) {
					break;
				}
				if (!(wParam & static_cast<int>(TabCompletion::Default))) {
					break;
				}
			}
			[[fallthrough]];

		default:
			return ScintillaBase::WndProc(iMessage, wParam, lParam);
		}
	} catch (std::bad_alloc &) {
		errorStatus = Status::BadAlloc;
	} catch (...) {
		errorStatus = Status::Failure;
	}
	return 0;
}

bool ScintillaWin::ValidCodePage(int codePage) const noexcept {
	return codePage == 0 || codePage == CpUtf8 || IsDBCSCodePage(codePage);
}

std::string ScintillaWin::UTF8FromEncoded(std::string_view encoded) const {
	if (IsUnicodeMode()) {
		return std::string(encoded);
	}
	// Pivot through wide string
	const std::wstring ws = StringDecode(encoded, CodePageOfDocument());
	return StringEncode(ws, CpUtf8);
}

std::string ScintillaWin::EncodedFromUTF8(std::string_view utf8) const {
	if (IsUnicodeMode()) {
		return std::string(utf8);
	}
	// Pivot through wide string
	const std::wstring ws = StringDecode(utf8, CpUtf8);
	return StringEncode(ws, CodePageOfDocument());
}

sptr_t ScintillaWin::DefWndProc(Message iMessage, uptr_t wParam, sptr_t lParam) noexcept {
	return ::DefWindowProc(MainHWND(), static_cast<unsigned int>(iMessage), wParam, lParam);
}

bool ScintillaWin::FineTickerRunning(TickReason reason) const noexcept {
	return timers[static_cast<size_t>(reason)] != 0;
}

void ScintillaWin::FineTickerStart(TickReason reason, int millis, int tolerance) noexcept {
	FineTickerCancel(reason);
	const UINT_PTR reasonIndex = static_cast<UINT_PTR>(reason);
	const UINT_PTR eventID = static_cast<UINT_PTR>(fineTimerStart) + reasonIndex;
#if _WIN32_WINNT >= _WIN32_WINNT_WIN8
	if (tolerance) {
		timers[reasonIndex] = ::SetCoalescableTimer(MainHWND(), eventID, millis, nullptr, tolerance);
	} else {
		timers[reasonIndex] = ::SetTimer(MainHWND(), eventID, millis, nullptr);
	}
#else
	if (SetCoalescableTimerFn && tolerance) {
		timers[reasonIndex] = SetCoalescableTimerFn(MainHWND(), eventID, millis, nullptr, tolerance);
	} else {
		timers[reasonIndex] = ::SetTimer(MainHWND(), eventID, millis, nullptr);
	}
#endif
}

void ScintillaWin::FineTickerCancel(TickReason reason) noexcept {
	const UINT_PTR reasonIndex = static_cast<UINT_PTR>(reason);
	if (timers[reasonIndex]) {
		::KillTimer(MainHWND(), timers[reasonIndex]);
		timers[reasonIndex] = 0;
	}
}

bool ScintillaWin::SetIdle(bool on) noexcept {
	// On Win32 the Idler is implemented as a Timer on the Scintilla window.  This
	// takes advantage of the fact that WM_TIMER messages are very low priority,
	// and are only posted when the message queue is empty, i.e. during idle time.
	if (idler.state != on) {
		if (on) {
			constexpr UINT waitTimeMillis = 10;
			idler.idlerID = ::SetTimer(MainHWND(), idleTimerID, waitTimeMillis, nullptr)
				? AsPointer<IdlerID>(static_cast<UINT_PTR>(idleTimerID)) : nullptr;
		} else {
			::KillTimer(MainHWND(), AsInteger<uptr_t>(idler.idlerID));
			idler.idlerID = nullptr;
		}
		idler.state = idler.idlerID != nullptr;
	}
	return idler.state;
}

void ScintillaWin::IdleWork() {
	styleIdleInQueue = false;
	Editor::IdleWork();
}

void ScintillaWin::QueueIdleWork(WorkItems items, Sci::Position upTo) noexcept {
	Editor::QueueIdleWork(items, upTo);
	if (!styleIdleInQueue) {
		if (PostMessage(MainHWND(), SC_WORK_IDLE, 0, 0)) {
			styleIdleInQueue = true;
		}
	}
}

void ScintillaWin::SetMouseCapture(bool on) noexcept {
	if (mouseDownCaptures) {
		if (on) {
			::SetCapture(MainHWND());
		} else {
			::ReleaseCapture();
		}
	}
	capturedMouse = on;
}

bool ScintillaWin::HaveMouseCapture() const noexcept {
	// Cannot just see if GetCapture is this window as the scroll bar also sets capture for the window
	return capturedMouse;
	//return capturedMouse && (::GetCapture() == MainHWND());
}

void ScintillaWin::SetTrackMouseLeaveEvent(bool on) noexcept {
	if (on && !trackedMouseLeave) {
		TRACKMOUSEEVENT tme {};
		tme.cbSize = sizeof(tme);
		tme.dwFlags = TME_LEAVE;
		tme.hwndTrack = MainHWND();
		tme.dwHoverTime = HOVER_DEFAULT;	// Unused but triggers Dr. Memory if not initialized
		TrackMouseEvent(&tme);
	}
	trackedMouseLeave = on;
}

void ScintillaWin::HideCursorIfPreferred() noexcept {
	// SPI_GETMOUSEVANISH from OS.
	if (typingWithoutCursor && !cursorIsHidden) {
		::SetCursor({});
		cursorIsHidden = true;
	}
}

void ScintillaWin::UpdateBaseElements() noexcept {
	struct ElementToIndex { Element element; int nIndex; };
	constexpr ElementToIndex eti[] = {
		{ Element::List, COLOR_WINDOWTEXT },
		{ Element::ListBack, COLOR_WINDOW },
		{ Element::ListSelected, COLOR_HIGHLIGHTTEXT },
		{ Element::ListSelectedBack, COLOR_HIGHLIGHT },
	};
	bool changed = false;
	for (const ElementToIndex &ei : eti) {
		if (vs.SetElementBase(ei.element, ColourFromSys(ei.nIndex))) {
			changed = true;
		}
	}
	if (changed) {
		Redraw();
	}
}

bool ScintillaWin::PaintContains(PRectangle rc) const noexcept {
	if (paintState == PaintState::painting) {
		return BoundsContains(rcPaint, hRgnUpdate, rc);
	}
	return true;
}

void ScintillaWin::ScrollText(Sci::Line /* linesToMove */) {
	//Platform::DebugPrintf("ScintillaWin::ScrollText %d\n", linesToMove);
	//::ScrollWindow(MainHWND(), 0,
	//	vs.lineHeight * linesToMove, 0, 0);
	//::UpdateWindow(MainHWND());
	Redraw();
	UpdateSystemCaret();
}

void ScintillaWin::NotifyCaretMove() noexcept {
	NotifyWinEvent(EVENT_OBJECT_LOCATIONCHANGE, MainHWND(), OBJID_CARET, CHILDID_SELF);
}

void ScintillaWin::UpdateSystemCaret() {
	if (hasFocus) {
		if (pdoc->TentativeActive()) {
			// ongoing inline mode IME composition, don't inform IME of system caret position.
			// fix candidate window for Google Japanese IME moved on typing on Win7.
			return;
		}
		if (HasCaretSizeChanged()) {
			DestroySystemCaret();
			CreateSystemCaret();
		}
		const Point pos = PointMainCaret();
		::SetCaretPos(static_cast<int>(pos.x), static_cast<int>(pos.y));
	}
}

BOOL ScintillaWin::IsVisible() const noexcept {
	return GetWindowStyle(MainHWND()) & WS_VISIBLE;
}

int ScintillaWin::SetScrollInfo(int nBar, LPCSCROLLINFO lpsi, BOOL bRedraw) const noexcept {
	return ::SetScrollInfo(MainHWND(), nBar, lpsi, bRedraw);
}

bool ScintillaWin::GetScrollInfo(int nBar, LPSCROLLINFO lpsi) const noexcept {
	return ::GetScrollInfo(MainHWND(), nBar, lpsi);
}

// Change the scroll position but avoid repaint if changing to same value
void ScintillaWin::ChangeScrollPos(int barType, Sci::Position pos) {
	if (!IsVisible()) {
		return;
	}

	SCROLLINFO sci {};
 	sci.cbSize = sizeof(sci);
	sci.fMask = SIF_POS;
	GetScrollInfo(barType, &sci);
	if (sci.nPos != pos) {
		DwellEnd(true);
		sci.nPos = static_cast<int>(pos);
		SetScrollInfo(barType, &sci, TRUE);
	}
}

void ScintillaWin::SetVerticalScrollPos() {
	Editor::SetVerticalScrollPos();
	ChangeScrollPos(SB_VERT, topLine);
}

void ScintillaWin::SetHorizontalScrollPos() {
	ChangeScrollPos(SB_HORZ, xOffset);
}

bool ScintillaWin::ChangeScrollRange(int nBar, int nMin, int nMax, UINT nPage) const noexcept {
	SCROLLINFO sci = { sizeof(sci), SIF_PAGE | SIF_RANGE, 0, 0, 0, 0, 0 };
	GetScrollInfo(nBar, &sci);
	if ((sci.nMin != nMin) || (sci.nMax != nMax) ||	(sci.nPage != nPage)) {
		sci.nMin = nMin;
		sci.nMax = nMax;
		sci.nPage = nPage;
		SetScrollInfo(nBar, &sci, TRUE);
		return true;
	}
	return false;
}

void ScintillaWin::HorizontalScrollToClamped(int xPos) {
	const HorizontalScrollRange range = GetHorizontalScrollRange();
	HorizontalScrollTo(std::clamp(xPos, 0, range.documentWidth - range.pageWidth + 1));
}

HorizontalScrollRange ScintillaWin::GetHorizontalScrollRange() const noexcept {
	const PRectangle rcText = GetTextRectangle();
	int pageWidth = static_cast<int>(rcText.Width());
	const int horizEndPreferred = std::max({ scrollWidth, pageWidth - 1, 0 });
	if (!horizontalScrollBarVisible || Wrapping()) {
		pageWidth = horizEndPreferred + 1;
	}
	return { pageWidth, horizEndPreferred };
}

bool ScintillaWin::ModifyScrollBars(Sci::Line nMax, Sci::Line nPage) {
	if (!IsVisible()) {
		return false;
	}

	const Sci::Line vertEndPreferred = nMax;
	if (!verticalScrollBarVisible) {
		nPage = vertEndPreferred + 1;
	}

	bool modified = ChangeScrollRange(SB_VERT, 0, static_cast<int>(vertEndPreferred), static_cast<unsigned int>(nPage));
	const HorizontalScrollRange range = GetHorizontalScrollRange();
	if (ChangeScrollRange(SB_HORZ, 0, range.documentWidth, range.pageWidth)) {
		modified = true;
		if (scrollWidth < range.pageWidth) {
			HorizontalScrollTo(0);
		}
	}
	return modified;
}

void ScintillaWin::NotifyChange() noexcept {
	::SendMessage(::GetParent(MainHWND()), WM_COMMAND,
		MAKEWPARAM(GetCtrlID(), FocusChange::Change),
		AsInteger<LPARAM>(MainHWND()));
}

void ScintillaWin::NotifyFocus(bool focus) {
	if (commandEvents) {
		::SendMessage(::GetParent(MainHWND()), WM_COMMAND,
			MAKEWPARAM(GetCtrlID(), focus ? FocusChange::Setfocus : FocusChange::Killfocus),
			AsInteger<LPARAM>(MainHWND()));
	}
	Editor::NotifyFocus(focus);
}

void ScintillaWin::SetCtrlID(int identifier) noexcept {
	::SetWindowID(MainHWND(), identifier);
}

int ScintillaWin::GetCtrlID() const noexcept {
	return ::GetDlgCtrlID(MainHWND());
}

void ScintillaWin::NotifyParent(NotificationData &scn) const noexcept {
	scn.nmhdr.hwndFrom = MainHWND();
	scn.nmhdr.idFrom = GetCtrlID();
	::SendMessage(::GetParent(MainHWND()), WM_NOTIFY,
		scn.nmhdr.idFrom, AsInteger<LPARAM>(&scn));
}

void ScintillaWin::NotifyDoubleClick(Point pt, KeyMod modifiers) {
	//Platform::DebugPrintf("ScintillaWin Double click 0\n");
	ScintillaBase::NotifyDoubleClick(pt, modifiers);
	// Send myself a WM_LBUTTONDBLCLK, so the container can handle it too.
	const POINT point = POINTFromPoint(pt);
	::SendMessage(MainHWND(),
		WM_LBUTTONDBLCLK,
		FlagSet(modifiers, KeyMod::Shift) ? MK_SHIFT : 0,
		MAKELPARAM(point.x, point.y));
}

void Editor::BeginBatchUpdate() noexcept {
	++batchUpdateDepth;
	if (batchUpdateDepth == 1) {
		batchUpdateState.modEventMask = modEventMask;
		modEventMask = ModificationFlags::None;
		batchUpdateState.actions = pdoc->UndoActions();
		batchUpdateState.lines = pdoc->LinesTotal();
		::SendMessage(HwndFromWindow(wMain), WM_SETREDRAW, FALSE, 0);
	}
}

void Editor::EndBatchUpdate() noexcept {
	--batchUpdateDepth;
	if (batchUpdateDepth == 0) {
		modEventMask = batchUpdateState.modEventMask;
		::SendMessage(HwndFromWindow(wMain), WM_SETREDRAW, TRUE, 0);
		::InvalidateRect(HwndFromWindow(wMain), nullptr, TRUE);
		if (batchUpdateState.actions != pdoc->UndoActions()) {
			NotificationData scn = {};
			scn.nmhdr.code = Notification::Modified;
			scn.linesAdded = pdoc->LinesTotal() - batchUpdateState.lines;
			NotifyParent(scn);
		}
	}
}

namespace {

constexpr unsigned int safeFoldingSize = 20;
constexpr uint8_t highByteFirst = 0x80;
constexpr uint8_t highByteLast = 0xFF;
constexpr uint8_t minTrailByte = 0x31;

// CreateFoldMap creates a fold map by calling platform APIs so will differ between platforms.
void CreateFoldMap(int codePage, FoldMap &foldingMap) {
	for (unsigned char byte1 = highByteFirst + 1; byte1 < highByteLast; byte1++) {
		if (DBCSIsLeadByte(codePage, byte1)) {
			for (unsigned char byte2 = minTrailByte; byte2 < highByteLast; byte2++) {
				if (DBCSIsTrailByte(codePage, byte2)) {
					const char sCharacter[2] = { static_cast<char>(byte1), static_cast<char>(byte2) };
					wchar_t codePoint[4]{};
					const int lenUni = ::MultiByteToWideChar(codePage, MB_ERR_INVALID_CHARS, sCharacter, 2, codePoint, _countof(codePoint));
					if (lenUni == 1 && codePoint[0]) {
						// DBCS pair must produce a single Unicode BMP code point
						// Could create a DBCS -> Unicode conversion map here
						const char *foldedUTF8 = CaseConvert(codePoint[0], CaseConversion::fold);
						if (foldedUTF8) {
							wchar_t wFolded[safeFoldingSize];
							const size_t charsConverted = UTF16FromUTF8(foldedUTF8, wFolded, std::size(wFolded));
							char back[safeFoldingSize];
							const int lengthBack = MultiByteFromWideChar(codePage, std::wstring_view(wFolded, charsConverted),
								back, std::size(back));
							if (lengthBack == 2) {
								// Only allow cases where input length and folded length are both 2
								const uint16_t index = DBCSIndex(byte1, byte2);
								foldingMap[index] = { back[0], back[1] };
							}
						}
					}
				}
			}
		}
	}
}

class CaseFolderDBCS final : public CaseFolderTable {
	// Allocate the expandable storage here so that it does not need to be reallocated
	// for each call to Fold.
	FoldMap foldingMap;
	UINT cp;
public:
	explicit CaseFolderDBCS(UINT cp_): cp{cp_} {
		// const ElapsedPeriod period;
		CreateFoldMap(cp, foldingMap);
		// const double duration = period.Duration()*1e3;
		// printf("%s(%u) duration=%.6f\n", __func__, cp, duration);
	}
	size_t Fold(char *folded, size_t sizeFolded, const char *mixed, size_t lenMixed) const override;
};

size_t CaseFolderDBCS::Fold(char *folded, size_t sizeFolded, const char *mixed, size_t lenMixed) const {
	// This loop outputs the same length as input as for each char 1-byte -> 1-byte; 2-byte -> 2-byte
	size_t lenOut = 0;
	for (size_t i = 0; i < lenMixed; ) {
		const ptrdiff_t lenLeft = lenMixed - i;
		const char ch = mixed[i++];
		if ((lenLeft >= 2) && DBCSIsLeadByte(cp, ch) && ((lenOut + 2) <= sizeFolded)) {
			i++;
			const char ch2 = mixed[i];
			const uint16_t ind = DBCSIndex(ch, ch2);
			const char *pair = foldingMap.at(ind).chars;
			if (pair[0]) {
				folded[lenOut++] = pair[0];
				folded[lenOut++] = pair[1];
			} else {
				folded[lenOut++] = ch;
				folded[lenOut++] = ch2;
			}
		} else if ((lenOut + 1) <= sizeFolded) {
			const unsigned char uch = ch;
			folded[lenOut++] = mapping[uch];
		}
	}

	return lenOut;
}

}

std::unique_ptr<CaseFolder> ScintillaWin::CaseFolderForEncoding() {
	const UINT cpDest = CodePageOfDocument();
	if (cpDest == CpUtf8) {
		return std::make_unique<CaseFolderUnicode>();
	}
	if (pdoc->dbcsCodePage) {
		return std::make_unique<CaseFolderDBCS>(cpDest);
	}
	std::unique_ptr<CaseFolderTable> pcf = std::make_unique<CaseFolderTable>();
	// Only for single byte encodings
	for (int i = highByteFirst; i <= highByteLast; i++) {
		const char sCharacter[2] = {static_cast<char>(i), '\0'};
		wchar_t wCharacter[safeFoldingSize];
		const unsigned int lengthUTF16 = WideCharFromMultiByte(cpDest, std::string_view(sCharacter, 1),
			wCharacter, std::size(wCharacter));
		if (lengthUTF16 == 1) {
			const char *caseFolded = CaseConvert(wCharacter[0], CaseConversion::fold);
			if (caseFolded) {
				wchar_t wLower[safeFoldingSize];
				const size_t charsConverted = UTF16FromUTF8(std::string_view(caseFolded),
					wLower, std::size(wLower));
				if (charsConverted == 1) {
					char sCharacterLowered[safeFoldingSize];
					const unsigned int lengthConverted = MultiByteFromWideChar(cpDest,
						std::wstring_view(wLower, charsConverted),
						sCharacterLowered, std::size(sCharacterLowered));
					if ((lengthConverted == 1) && (sCharacter[0] != sCharacterLowered[0])) {
						pcf->SetTranslation(sCharacter[0], sCharacterLowered[0]);
					}
				}
			}
		}
	}
	return pcf;
}

std::string ScintillaWin::CaseMapString(const std::string &s, CaseMapping caseMapping) const {
	if (s.empty() || (caseMapping == CaseMapping::same))
		return s;

	const UINT cpDoc = CodePageOfDocument();
	if (cpDoc == CpUtf8) {
		return CaseConvertString(s, (caseMapping == CaseMapping::upper) ? CaseConversion::upper : CaseConversion::lower);
	}

	// Change text to UTF-16
	const std::wstring wsText = StringDecode(s, cpDoc);

	const DWORD mapFlags = LCMAP_LINGUISTIC_CASING |
		((caseMapping == CaseMapping::upper) ? LCMAP_UPPERCASE : LCMAP_LOWERCASE);

	// Change case
	const std::wstring wsConverted = StringMapCase(wsText, mapFlags);

	// Change back to document encoding
	std::string sConverted = StringEncode(wsConverted, cpDoc);

	return sConverted;
}

void ScintillaWin::Copy(bool asBinary) const {
	//Platform::DebugPrintf("Copy\n");
	if (!sel.Empty()) {
		SelectionText selectedText;
		selectedText.asBinary = asBinary;
		CopySelectionRange(selectedText);
		CopyToClipboard(selectedText);
	}
}

bool ScintillaWin::CanPaste() noexcept {
	if (!Editor::CanPaste())
		return false;
#if DebugCopyAsRichTextFormat
	if (::IsClipboardFormatAvailable(cfRTF)) {
		return true;
	}
#endif
	return ::IsClipboardFormatAvailable(CF_UNICODETEXT);
}

namespace {

class GlobalMemory {
	HGLOBAL hand {};
public:
	void *ptr {};
	GlobalMemory() noexcept = default;
	explicit GlobalMemory(HGLOBAL hand_) noexcept : hand(hand_) {
		if (hand) {
			ptr = ::GlobalLock(hand);
		}
	}
	// Deleted so GlobalMemory objects can not be copied.
	GlobalMemory(const GlobalMemory &) = delete;
	GlobalMemory(GlobalMemory &&) = delete;
	GlobalMemory &operator=(const GlobalMemory &) = delete;
	GlobalMemory &operator=(GlobalMemory &&) = delete;
	~GlobalMemory() {
		assert(!ptr);
		assert(!hand);
	}
	void Allocate(size_t bytes) noexcept {
		assert(!hand);
		hand = ::GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
		if (hand) {
			ptr = ::GlobalLock(hand);
		}
	}
	HGLOBAL Unlock() noexcept {
		assert(ptr);
		HGLOBAL handCopy = hand;
		::GlobalUnlock(hand);
		ptr = nullptr;
		hand = nullptr;
		return handCopy;
	}
	void SetClip(UINT uFormat) noexcept {
		::SetClipboardData(uFormat, Unlock());
	}
	explicit operator bool() const noexcept {
		return ptr != nullptr;
	}
	[[nodiscard]] SIZE_T Size() noexcept {
		return ::GlobalSize(hand);
	}
};

// OpenClipboard may fail if another application has opened the clipboard.
// Try up to 8 times, with an initial delay of 1 ms and an exponential back off
// for a maximum total delay of 127 ms (1+2+4+8+16+32+64).
bool OpenClipboardRetry(HWND hwnd) noexcept {
	constexpr int attempts = 8;
	for (int attempt = 0; attempt < attempts; attempt++) {
		if (attempt > 0) {
			::Sleep(1 << (attempt - 1));
		}
		if (::OpenClipboard(hwnd)) {
			return true;
		}
	}
	return false;
}

// Ensure every successful OpenClipboard is followed by a CloseClipboard.
class Clipboard {
	bool opened = false;
public:
	explicit Clipboard(HWND hwnd) noexcept : opened(::OpenClipboardRetry(hwnd)) {
	}
	// Deleted so Clipboard objects can not be copied.
	Clipboard(const Clipboard &) = delete;
	Clipboard(Clipboard &&) = delete;
	Clipboard &operator=(const Clipboard &) = delete;
	Clipboard &operator=(Clipboard &&) = delete;
	~Clipboard() noexcept {
		if (opened) {
			::CloseClipboard();
		}
	}
	explicit constexpr operator bool() const noexcept {
		return opened;
	}
};

inline bool IsValidFormatEtc(const FORMATETC *pFE) noexcept {
	return pFE->ptd == nullptr
		&& (pFE->dwAspect & DVASPECT_CONTENT) != 0
		&& pFE->lindex == -1
		&& (pFE->tymed & TYMED_HGLOBAL) != 0;
}

inline bool SupportedFormat(const FORMATETC *pFE) noexcept {
	return (pFE->cfFormat == CF_UNICODETEXT || pFE->cfFormat == CF_TEXT)
		&& IsValidFormatEtc(pFE);
}

}

void ScintillaWin::Paste(bool asBinary) {
	const Clipboard clipboard(MainHWND());
	if (!clipboard) {
		return;
	}

	const UndoGroup ug(pdoc);
	//EnumAllClipboardFormat("Paste");
	const bool isLine = SelectionEmpty() &&
		(::IsClipboardFormatAvailable(cfLineSelect) || ::IsClipboardFormatAvailable(cfVSLineTag));
	ClearSelection(multiPasteMode == MultiPaste::Each);
	bool isRectangular = (::IsClipboardFormatAvailable(cfColumnSelect) != 0);

	if (!isRectangular) {
		// Evaluate "Borland IDE Block Type" explicitly
		GlobalMemory memBorlandSelection(::GetClipboardData(cfBorlandIDEBlockType));
		if (memBorlandSelection) {
			isRectangular = (memBorlandSelection.Size() == 1) && (static_cast<BYTE *>(memBorlandSelection.ptr)[0] == 0x02);
			memBorlandSelection.Unlock();
		}
	}

	const PasteShape pasteShape = isRectangular ? PasteShape::rectangular : (isLine ? PasteShape::line : PasteShape::stream);

	if (asBinary) {
		// get data with CF_TEXT, decode and verify length information
		if (!asBinary) {
			Redraw();
			return;
		}
	}

#if DebugCopyAsRichTextFormat
	if (::IsClipboardFormatAvailable(cfRTF)) {
		GlobalMemory memUSelection(::GetClipboardData(cfRTF));
		if (const char *ptr = static_cast<const char *>(memUSelection.ptr)) {
			NewLine();
			InsertPasteShape(ptr, strlen(ptr), PasteShape::stream);
			memUSelection.Unlock();
			NewLine();
		}
	}
#endif

	// Use CF_UNICODETEXT if available
	GlobalMemory memUSelection(::GetClipboardData(CF_UNICODETEXT));
	if (const wchar_t *uptr = static_cast<const wchar_t *>(memUSelection.ptr)) {
		const std::string putf = EncodeWString(uptr);
		InsertPasteShape(putf.c_str(), putf.length(), pasteShape);
		memUSelection.Unlock();
	}
	Redraw();
}

void ScintillaWin::CreateCallTipWindow(PRectangle) noexcept {
	if (!ct.wCallTip.Created()) {
		HWND wnd = ::CreateWindow(callClassName, callClassName,
			WS_POPUP, 100, 100, 150, 20,
			MainHWND(), {},
			GetWindowInstance(MainHWND()),
			this);
		ct.wCallTip = wnd;
		ct.wDraw = wnd;
	}
}

#if SCI_EnablePopupMenu
void ScintillaWin::AddToPopUp(const char *label, int cmd, bool enabled) noexcept {
	HMENU hmenuPopup = static_cast<HMENU>(popup.GetID());
	if (!label[0])
		::AppendMenuA(hmenuPopup, MF_SEPARATOR, 0, "");
	else if (enabled)
		::AppendMenuA(hmenuPopup, MF_STRING, cmd, label);
	else
		::AppendMenuA(hmenuPopup, MF_STRING | MF_DISABLED | MF_GRAYED, cmd, label);
}
#endif

void ScintillaWin::ClaimSelection() noexcept {
	// Windows does not have a primary selection
}

/// Implement IUnknown
STDMETHODIMP FormatEnumerator::QueryInterface(REFIID riid, PVOID *ppv) noexcept {
	//Platform::DebugPrintf("EFE QI");
	*ppv = nullptr;
	if (riid == IID_IUnknown || riid == IID_IEnumFORMATETC) {
		*ppv = this;
	} else {
		return E_NOINTERFACE;
	}
	AddRef();
	return S_OK;
}
STDMETHODIMP_(ULONG)FormatEnumerator::AddRef() noexcept {
	return ++ref;
}
STDMETHODIMP_(ULONG)FormatEnumerator::Release() noexcept {
	const ULONG refs = --ref;
	if (refs == 0) {
		delete this;
	}
	return refs;
}

/// Implement IEnumFORMATETC
STDMETHODIMP FormatEnumerator::Next(ULONG celt, FORMATETC *rgelt, ULONG *pceltFetched) noexcept {
	if (!rgelt) return E_POINTER;
	ULONG putPos = 0;
	while ((pos < formats.size()) && (putPos < celt)) {
		rgelt->cfFormat = formats[pos];
		rgelt->ptd = nullptr;
		rgelt->dwAspect = DVASPECT_CONTENT;
		rgelt->lindex = -1;
		rgelt->tymed = TYMED_HGLOBAL;
		rgelt++;
		pos++;
		putPos++;
	}
	if (pceltFetched)
		*pceltFetched = putPos;
	return putPos ? S_OK : S_FALSE;
}
STDMETHODIMP FormatEnumerator::Skip(ULONG celt) noexcept {
	pos += celt;
	return S_OK;
}
STDMETHODIMP FormatEnumerator::Reset() noexcept {
	pos = 0;
	return S_OK;
}
STDMETHODIMP FormatEnumerator::Clone(IEnumFORMATETC **ppenum) {
	FormatEnumerator *pfe;
	try {
		pfe = new FormatEnumerator(pos, formats.data(), formats.size());
	} catch (...) {
		return E_OUTOFMEMORY;
	}
	return pfe->QueryInterface(IID_IEnumFORMATETC, AsPPVArgs(ppenum));
}

FormatEnumerator::FormatEnumerator(ULONG pos_, const CLIPFORMAT formats_[], size_t formatsLen_) {
	ref = 0;   // First QI adds first reference...
	pos = pos_;
	formats.insert(formats.begin(), formats_, formats_ + formatsLen_);
}

/// Implement IUnknown
STDMETHODIMP DropSource::QueryInterface(REFIID riid, PVOID *ppv) noexcept {
	return sci->QueryInterface(riid, ppv);
}
STDMETHODIMP_(ULONG)DropSource::AddRef() noexcept {
	return sci->AddRef();
}
STDMETHODIMP_(ULONG)DropSource::Release() noexcept {
	return sci->Release();
}

/// Implement IDropSource
STDMETHODIMP DropSource::QueryContinueDrag(BOOL fEsc, DWORD grfKeyState) noexcept {
	if (fEsc)
		return DRAGDROP_S_CANCEL;
	if (!(grfKeyState & MK_LBUTTON))
		return DRAGDROP_S_DROP;
	return S_OK;
}
STDMETHODIMP DropSource::GiveFeedback(DWORD) noexcept {
	return DRAGDROP_S_USEDEFAULTCURSORS;
}

/// Implement IUnkown
STDMETHODIMP DataObject::QueryInterface(REFIID riid, PVOID *ppv) noexcept {
	//Platform::DebugPrintf("DO QI %p\n", this);
	return sci->QueryInterface(riid, ppv);
}
STDMETHODIMP_(ULONG)DataObject::AddRef() noexcept {
	return sci->AddRef();
}
STDMETHODIMP_(ULONG)DataObject::Release() noexcept {
	return sci->Release();
}

/// Implement IDataObject
STDMETHODIMP DataObject::GetData(FORMATETC *pFEIn, STGMEDIUM *pSTM) {
	return sci->GetData(pFEIn, pSTM);
}

STDMETHODIMP DataObject::GetDataHere(FORMATETC *, STGMEDIUM *) noexcept {
	//Platform::DebugPrintf("DOB GetDataHere\n");
	return E_NOTIMPL;
}

STDMETHODIMP DataObject::QueryGetData(FORMATETC *pFE) noexcept {
	if (sci->DragIsRectangularOK(pFE->cfFormat) && IsValidFormatEtc(pFE)) {
		return S_OK;
	}

	return SupportedFormat(pFE) ? S_OK : S_FALSE;
}

STDMETHODIMP DataObject::GetCanonicalFormatEtc(FORMATETC *, FORMATETC *pFEOut) noexcept {
	//Platform::DebugPrintf("DOB GetCanon\n");
	pFEOut->cfFormat = CF_UNICODETEXT;
	pFEOut->ptd = nullptr;
	pFEOut->dwAspect = DVASPECT_CONTENT;
	pFEOut->lindex = -1;
	pFEOut->tymed = TYMED_HGLOBAL;
	return S_OK;
}

STDMETHODIMP DataObject::SetData(FORMATETC *, STGMEDIUM *, BOOL) noexcept {
	//Platform::DebugPrintf("DOB SetData\n");
	return E_FAIL;
}

STDMETHODIMP DataObject::EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC **ppEnum) {
	try {
		//Platform::DebugPrintf("DOB EnumFormatEtc %lu\n", dwDirection);
		if (dwDirection != DATADIR_GET) {
			*ppEnum = nullptr;
			return E_FAIL;
		}

		const CLIPFORMAT formats[] = { CF_UNICODETEXT, CF_TEXT };
		FormatEnumerator *pfe = new FormatEnumerator(0, formats, std::size(formats));
		return pfe->QueryInterface(IID_IEnumFORMATETC, AsPPVArgs(ppEnum));
	} catch (std::bad_alloc &) {
		sci->errorStatus = Status::BadAlloc;
		return E_OUTOFMEMORY;
	} catch (...) {
		sci->errorStatus = Status::Failure;
		return E_FAIL;
	}
}

STDMETHODIMP DataObject::DAdvise(FORMATETC *, DWORD, IAdviseSink *, PDWORD) noexcept {
	//Platform::DebugPrintf("DOB DAdvise\n");
	return E_FAIL;
}

STDMETHODIMP DataObject::DUnadvise(DWORD) noexcept {
	//Platform::DebugPrintf("DOB DUnadvise\n");
	return E_FAIL;
}

STDMETHODIMP DataObject::EnumDAdvise(IEnumSTATDATA **) noexcept {
	//Platform::DebugPrintf("DOB EnumDAdvise\n");
	return E_FAIL;
}

/// Implement IUnknown
STDMETHODIMP DropTarget::QueryInterface(REFIID riid, PVOID *ppv) noexcept {
	//Platform::DebugPrintf("DT QI %p\n", this);
	return sci->QueryInterface(riid, ppv);
}
STDMETHODIMP_(ULONG)DropTarget::AddRef() noexcept {
	return sci->AddRef();
}
STDMETHODIMP_(ULONG)DropTarget::Release() noexcept {
	return sci->Release();
}

/// Implement IDropTarget by forwarding to Scintilla
STDMETHODIMP DropTarget::DragEnter(LPDATAOBJECT pIDataSource, DWORD grfKeyState, POINTL pt, PDWORD pdwEffect) {
	return sci->DragEnter(pIDataSource, grfKeyState, pt, pdwEffect);
}
STDMETHODIMP DropTarget::DragOver(DWORD grfKeyState, POINTL pt, PDWORD pdwEffect) {
	return sci->DragOver(grfKeyState, pt, pdwEffect);
}
STDMETHODIMP DropTarget::DragLeave() {
	return sci->DragLeave();
}
STDMETHODIMP DropTarget::Drop(LPDATAOBJECT pIDataSource, DWORD grfKeyState, POINTL pt, PDWORD pdwEffect) {
	return sci->Drop(pIDataSource, grfKeyState, pt, pdwEffect);
}

/**
 * DBCS: support Input Method Editor (IME).
 * Called when IME Window opened.
 */
void ScintillaWin::ImeStartComposition() {
	if (caret.active) {
		// Move IME Window to current caret position
		const IMContext imc(MainHWND());
		if (!imc) {
			return;
		}

		imc.SetCompositionWindow(PointMainCaret());

		// Set font of IME window to same as surrounded text.
		if (stylesValid) {
			// Since the style creation code has been made platform independent,
			// The logfont for the IME is recreated here.
			imc.SetCompositionFont(vs, pdoc->StyleIndexAt(sel.MainCaret()), dpi);
		}
		// Caret is displayed in IME window. So, caret in Scintilla is useless.
		DropCaret();
	}
}

/** Called when IME Window closed.
* TODO: see Chromium's InputMethodWinImm32::OnImeEndComposition().
*/
void ScintillaWin::ImeEndComposition() {
	// clear IME composition state.
	view.imeCaretBlockOverride = false;
	pdoc->TentativeUndo();
	ShowCaretAtCurrentPosition();
}

LRESULT ScintillaWin::ImeOnReconvert(LPARAM lParam) {
	// Reconversion on windows limits within one line without eol.
	// Look around:   baseStart  <--  (|mainStart|  -- mainEnd)  --> baseEnd.
	const Sci::Position mainStart = sel.RangeMain().Start().Position();
	const Sci::Position mainEnd = sel.RangeMain().End().Position();
	const Sci::Line curLine = pdoc->SciLineFromPosition(mainStart);
	if (curLine != pdoc->SciLineFromPosition(mainEnd)) {
		return 0;
	}
	const Sci::Position baseStart = pdoc->LineStart(curLine);
	const Sci::Position baseEnd = pdoc->LineEnd(curLine);
	if ((baseStart == baseEnd) || (mainEnd > baseEnd)) {
		return 0;
	}

	const UINT codePage = CodePageOfDocument();
	const std::wstring rcFeed = StringDecode(RangeText(baseStart, baseEnd), codePage);
	const DWORD rcFeedLen = static_cast<DWORD>(rcFeed.length()) * sizeof(wchar_t);
	const DWORD rcSize = sizeof(RECONVERTSTRING) + rcFeedLen + sizeof(wchar_t);

	RECONVERTSTRING *rc = AsPointer<RECONVERTSTRING *>(lParam);
	if (!rc) {
		return rcSize; // Immediately be back with rcSize of memory block.
	}

	wchar_t *rcFeedStart = reinterpret_cast<wchar_t*>(rc + 1);
	memcpy(rcFeedStart, rcFeed.data(), rcFeedLen);

	const std::string rcCompString = RangeText(mainStart, mainEnd);
	const std::wstring rcCompWstring = StringDecode(rcCompString, codePage);
	const std::string rcCompStart = RangeText(baseStart, mainStart);
	const std::wstring rcCompWstart = StringDecode(rcCompStart, codePage);

	// Map selection to dwCompStr.
	// No selection assumes current caret as rcCompString without length.
	rc->dwVersion = 0; // It should be absolutely 0.
	rc->dwStrLen = static_cast<DWORD>(rcFeed.length());
	rc->dwStrOffset = sizeof(RECONVERTSTRING);
	rc->dwCompStrLen = static_cast<DWORD>(rcCompWstring.length());
	rc->dwCompStrOffset = static_cast<DWORD>(rcCompWstart.length()) * sizeof(wchar_t);
	rc->dwTargetStrLen = rc->dwCompStrLen;
	rc->dwTargetStrOffset = rc->dwCompStrOffset;

	const IMContext imc(MainHWND());
	if (!imc) {
		return 0;
	}

	if (!imc.SetCompositionString(SCS_QUERYRECONVERTSTRING, rc, rcSize)) {
		return 0;
	}

	// No selection asks IME to fill target fields with its own value.
	const size_t tgWlen = rc->dwTargetStrLen;
	const size_t tgWstart = rc->dwTargetStrOffset / sizeof(wchar_t);

	const std::string tgCompStart = StringEncode(rcFeed.substr(0, tgWstart), codePage);
	const std::string tgComp = StringEncode(rcFeed.substr(tgWstart, tgWlen), codePage);

	// No selection needs to adjust reconvert start position for IME set.
	const Sci::Position adjust = tgCompStart.length() - rcCompStart.length();
	const Sci::Position docCompLen = tgComp.length();

	// Make place for next composition string to sit in.
	for (size_t r = 0; r < sel.Count(); r++) {
		const Sci::Position rBase = sel.Range(r).Start().Position();
		const Sci::Position docCompStart = rBase + adjust;

		if (inOverstrike) { // the docCompLen of bytes will be overstriked.
			sel.Range(r) = SelectionRange(docCompStart);
		} else {
			// Ensure docCompStart+docCompLen be not beyond lineEnd.
			// since docCompLen by byte might break eol.
			const Sci::Position lineEnd = pdoc->LineEndPosition(rBase);
			const Sci::Position overflow = (docCompStart + docCompLen) - lineEnd;
			if (overflow > 0) {
				pdoc->DeleteChars(docCompStart, docCompLen - overflow);
			} else {
				pdoc->DeleteChars(docCompStart, docCompLen);
			}
		}
	}
	// Immediately Target Input or candidate box choice with GCS_COMPSTR.
	return rcSize;
}

LRESULT ScintillaWin::ImeOnDocumentFeed(LPARAM lParam) const {
	// This is called while typing preedit string in.
	// So there is no selection.
	// Limit feed within one line without EOL.
	// Look around:   lineStart |<--  |compStart| - caret - compEnd|  -->| lineEnd.

	const Sci::Position curPos = CurrentPosition();
	const Sci::Line curLine = pdoc->SciLineFromPosition(curPos);
	const Sci::Position lineStart = pdoc->LineStart(curLine);
	const Sci::Position lineEnd = pdoc->LineEnd(curLine);

	const std::wstring rcFeed = StringDecode(RangeText(lineStart, lineEnd), CodePageOfDocument());
	const size_t rcFeedLen = rcFeed.length() * sizeof(wchar_t);
	const size_t rcSize = sizeof(RECONVERTSTRING) + rcFeedLen + sizeof(wchar_t);

	RECONVERTSTRING *rc = AsPointer<RECONVERTSTRING *>(lParam);
	if (!rc) {
		return rcSize;
	}

	wchar_t *rcFeedStart = reinterpret_cast<wchar_t*>(rc + 1);
	memcpy(rcFeedStart, rcFeed.data(), rcFeedLen);

	const IMContext imc(MainHWND());
	if (!imc) {
		return 0;
	}

	DWORD compStrLen = 0;
	Sci::Position compStart = curPos;
	if (pdoc->TentativeActive()) {
		// rcFeed contains current composition string
		compStrLen = imc.GetCompositionStringLength(GCS_COMPSTR);
		const int imeCaretPos = imc.GetImeCaretPos();
		compStart = pdoc->GetRelativePositionUTF16(curPos, -imeCaretPos);
	}
	const Sci::Position compStrOffset = pdoc->CountUTF16(lineStart, compStart);

	// Fill in reconvert structure.
	// Let IME to decide what the target is.
	rc->dwVersion = 0; //constant
	rc->dwStrLen = static_cast<DWORD>(rcFeed.length());
	rc->dwStrOffset = sizeof(RECONVERTSTRING); //constant
	rc->dwCompStrLen = compStrLen;
	rc->dwCompStrOffset = static_cast<DWORD>(compStrOffset) * sizeof(wchar_t);
	rc->dwTargetStrLen = rc->dwCompStrLen;
	rc->dwTargetStrOffset = rc->dwCompStrOffset;

	return rcSize; // MS API says reconv structure to be returned.
}

void ScintillaWin::GetMouseParameters() noexcept {
	// mouse pointer size and colour may changed
	reverseArrowCursor.Invalidate();
	::SystemParametersInfo(SPI_GETMOUSEVANISH, 0, &typingWithoutCursor, 0);
	// This retrieves the number of lines per scroll as configured in the Mouse Properties sheet in Control Panel
	::SystemParametersInfo(SPI_GETWHEELSCROLLLINES, 0, &linesPerScroll, 0);
	if (!::SystemParametersInfo(SPI_GETWHEELSCROLLCHARS, 0, &charsPerScroll, 0)) {
		// no horizontal scrolling configuration on Windows XP
		charsPerScroll = (linesPerScroll == WHEEL_PAGESCROLL) ? 3 : linesPerScroll;
	}
}

void ScintillaWin::CopyToGlobal(GlobalMemory &gmUnicode, const SelectionText &selectedText, CopyEncoding encoding) const {
	const std::string_view svSelected(selectedText.Data(), selectedText.LengthWithTerminator());
	switch (encoding) {
	case CopyEncoding::Unicode: {
		// Convert to Unicode using the current Scintilla code page
		const UINT cpSrc = selectedText.codePage;
		const size_t uLen = WideCharLenFromMultiByte(cpSrc, svSelected);
		gmUnicode.Allocate(2 * uLen);
		if (gmUnicode) {
			WideCharFromMultiByte(cpSrc, svSelected, static_cast<wchar_t *>(gmUnicode.ptr), uLen);
		}
	}
	break;

	case CopyEncoding::Ansi: {
		std::string s;
		if (IsUnicodeMode()) {
			const std::wstring wsv = StringDecode(svSelected, CP_UTF8);
			s = StringEncode(wsv, CP_ACP);
		} else {
			// no need to convert selectedText to CP_ACP
			s = svSelected;
		}
		gmUnicode.Allocate(s.size() + 1);
		if (gmUnicode) {
			memcpy(gmUnicode.ptr, s.c_str(), s.size());
		}
	}
	break;

	case CopyEncoding::Binary:
		gmUnicode.Allocate(svSelected.size());
		if (gmUnicode) {
			memcpy(gmUnicode.ptr, svSelected.data(), svSelected.size() - 1);
		}
		break;
	}
}

void ScintillaWin::CopyToClipboard(const SelectionText &selectedText) const {
	const Clipboard clipboard(MainHWND());
	if (!clipboard) {
		return;
	}
	::EmptyClipboard();

	GlobalMemory uniText;
	CopyToGlobal(uniText, selectedText, selectedText.asBinary ? CopyEncoding::Binary : CopyEncoding::Unicode);

	if (uniText) {
		uniText.SetClip(selectedText.asBinary ? CF_TEXT : CF_UNICODETEXT);

		if (selectedText.asBinary) {
			// encode length information
		}
	}

	if (selectedText.rectangular) {
		::SetClipboardData(cfColumnSelect, {});

		GlobalMemory borlandSelection;
		borlandSelection.Allocate(1);
		if (borlandSelection) {
			static_cast<BYTE *>(borlandSelection.ptr)[0] = 0x02;
			borlandSelection.SetClip(cfBorlandIDEBlockType);
		}
	}

	if (selectedText.lineCopy) {
		::SetClipboardData(cfLineSelect, {});
		::SetClipboardData(cfVSLineTag, {});
	}

	// TODO: notify data loss
	//if (!selectedText.asBinary && ) {
	//}
}

void ScintillaWin::ScrollMessage(WPARAM wParam) {
	//DWORD dwStart = GetTickCount();
	//Platform::DebugPrintf("Scroll %x %d\n", wParam, lParam);

	SCROLLINFO sci = {};
	sci.cbSize = sizeof(sci);
	sci.fMask = SIF_ALL;

	GetScrollInfo(SB_VERT, &sci);

	//Platform::DebugPrintf("ScrollInfo %d mask=%x min=%d max=%d page=%d pos=%d track=%d\n", b, sci.fMask,
	//sci.nMin, sci.nMax, sci.nPage, sci.nPos, sci.nTrackPos);
	Sci::Line topLineNew = topLine;
	switch (LOWORD(wParam)) {
	case SB_LINEUP:
		topLineNew -= 1;
		break;
	case SB_LINEDOWN:
		topLineNew += 1;
		break;
	case SB_PAGEUP:
		topLineNew -= LinesToScroll();
		break;
	case SB_PAGEDOWN:
		topLineNew += LinesToScroll();
		break;
	case SB_TOP:
		topLineNew = 0;
		break;
	case SB_BOTTOM:
		topLineNew = MaxScrollPos();
		break;
	case SB_THUMBPOSITION:
		topLineNew = sci.nTrackPos;
		break;
	case SB_THUMBTRACK:
		topLineNew = sci.nTrackPos;
		break;
	default:
		break;
	}
	ScrollTo(topLineNew);
}

void ScintillaWin::HorizontalScrollMessage(WPARAM wParam) {
	int xPos = xOffset;
	const PRectangle rcText = GetTextRectangle();
	const int pageWidth = static_cast<int>(rcText.Width() * 2 / 3);
	constexpr int pixelsPerArrow = 20;
	switch (LOWORD(wParam)) {
	case SB_LINEUP:
		xPos -= pixelsPerArrow;
		break;
	case SB_LINEDOWN:	// May move past the logical end
		xPos += pixelsPerArrow;
		break;
	case SB_PAGEUP:
		xPos -= pageWidth;
		break;
	case SB_PAGEDOWN:
		xPos += pageWidth;
		break;
	case SB_TOP:
		xPos = 0;
		break;
	case SB_BOTTOM:
		xPos = scrollWidth;
		break;
	case SB_THUMBPOSITION:
	case SB_THUMBTRACK: {
		// Do NOT use wParam, its 16 bit and not enough for very long lines. It's still possible to overflow the 32 bit but you have to try harder =]
		SCROLLINFO si {};
		si.cbSize = sizeof(si);
		si.fMask = SIF_TRACKPOS;
		if (GetScrollInfo(SB_HORZ, &si)) {
			xPos = si.nTrackPos;
		}
	}
	break;
	default:
		break;
	}
	HorizontalScrollToClamped(xPos);
}

/**
 * Redraw all of text area.
 * This paint will not be abandoned.
 */
void ScintillaWin::FullPaint() {
	if ((technology == Technology::Default) || (technology == Technology::DirectWriteDC)) {
		HDC hdc = ::GetDC(MainHWND());
		FullPaintDC(hdc);
		::ReleaseDC(MainHWND(), hdc);
	} else {
		FullPaintDC({});
	}
}

/**
 * Redraw all text area on the specified DC.
 * This paint will not be abandoned.
 */
void ScintillaWin::FullPaintDC(HDC hdc) {
	paintState = PaintState::painting;
	rcPaint = GetClientRectangle();
	paintingAllText = true;
	PaintDC(hdc);
	paintState = PaintState::notPainting;
}

namespace {

inline bool CompareDevCap(HDC hdc, HDC hOtherDC, int nIndex) noexcept {
	return ::GetDeviceCaps(hdc, nIndex) == ::GetDeviceCaps(hOtherDC, nIndex);
}

}

bool ScintillaWin::IsCompatibleDC(HDC hOtherDC) const noexcept {
	HDC hdc = ::GetDC(MainHWND());
	const bool isCompatible =
		CompareDevCap(hdc, hOtherDC, TECHNOLOGY) &&
		CompareDevCap(hdc, hOtherDC, LOGPIXELSY) &&
		CompareDevCap(hdc, hOtherDC, LOGPIXELSX) &&
		CompareDevCap(hdc, hOtherDC, BITSPIXEL) &&
		CompareDevCap(hdc, hOtherDC, PLANES);
	::ReleaseDC(MainHWND(), hdc);
	return isCompatible;
}

// https://docs.microsoft.com/en-us/windows/desktop/api/oleidl/nf-oleidl-idroptarget-dragenter
DWORD ScintillaWin::EffectFromState(DWORD grfKeyState) const noexcept {
	// These are the Wordpad semantics.
	// DROPEFFECT_COPY not works for some applications like GitHub Atom.
	DWORD dwEffect = DROPEFFECT_MOVE;
#if 0
	if (inDragDrop == DragDrop::dragging)	// Internal defaults to move
		dwEffect = DROPEFFECT_MOVE;
	else
		dwEffect = DROPEFFECT_COPY;
	if ((grfKeyState & MK_CONTROL) && (grfKeyState & MK_SHIFT))
		dwEffect = DROPEFFECT_LINK;
	else
	if (grfKeyState & (MK_ALT | MK_SHIFT))
		dwEffect = DROPEFFECT_MOVE;
	else
#endif
	if (grfKeyState & MK_CONTROL)
		dwEffect = DROPEFFECT_COPY;
	return dwEffect;
}

/// Implement IUnknown
STDMETHODIMP ScintillaWin::QueryInterface(REFIID riid, PVOID *ppv) noexcept {
	*ppv = nullptr;
	if (riid == IID_IUnknown) {
		*ppv = &dt;
	} else if (riid == IID_IDropSource) {
		*ppv = &ds;
	} else if (riid == IID_IDropTarget) {
		*ppv = &dt;
	} else if (riid == IID_IDataObject) {
		*ppv = &dob;
	}
	if (!*ppv) {
		return E_NOINTERFACE;
	}
	return S_OK;
}

STDMETHODIMP_(ULONG) ScintillaWin::AddRef() noexcept {
	return 1;
}

STDMETHODIMP_(ULONG) ScintillaWin::Release() noexcept {
	return 1;
}

#if DebugDragAndDropDataFormat

namespace {

const char* GetStorageMediumType(DWORD tymed) noexcept {
	switch (tymed) {
	case TYMED_HGLOBAL:
		return "TYMED_HGLOBAL";
	case TYMED_FILE:
		return "TYMED_FILE";
	case TYMED_ISTREAM:
		return "TYMED_ISTREAM";
	case TYMED_ISTORAGE:
		return "TYMED_ISTORAGE";
	default:
		return "Unknown";
	}
}

const char* GetSourceFormatName(UINT fmt, char name[], int cchName) noexcept {
	const int len = RegisterClipboardTypeNameA(fmt, name, cchName);
	if (len <= 0) {
		switch (fmt) {
		case CF_TEXT:
			return "CF_TEXT";
		case CF_UNICODETEXT:
			return "CF_UNICODETEXT";
		case CF_HDROP:
			return "CF_HDROP";
		case CF_LOCALE:
			return "CF_LOCALE";
		case CF_OEMTEXT:
			return "CF_OEMTEXT";
		default:
			return "Unknown";
		}
	}

	return name;
}

}

// https://docs.microsoft.com/en-us/windows/desktop/api/objidl/nf-objidl-idataobject-enumformatetc
void ScintillaWin::EnumDataSourceFormat(const char *tag, LPDATAOBJECT pIDataSource) {
	IEnumFORMATETC *fmtEnum = nullptr;
	HRESULT hr = pIDataSource->EnumFormatEtc(DATADIR_GET, &fmtEnum);
	if (hr == S_OK && fmtEnum) {
		FORMATETC fetc[32] = {};
		ULONG fetched = 0;
		hr = fmtEnum->Next(sizeof(fetc) / sizeof(fetc[0]), fetc, &fetched);
		if (fetched > 0) {
			char name[1024];
			char buf[2048];
			for (ULONG i = 0; i < fetched; i++) {
				const CLIPFORMAT fmt = fetc[i].cfFormat;
				const DWORD tymed = fetc[i].tymed;
				const char *typeName = GetStorageMediumType(tymed);
				const char *fmtName = GetSourceFormatName(fmt, name, sizeof(name));
				const int len = sprintf(buf, "%s: fmt[%lu]=%u, 0x%04X; tymed=%lu, %s; name=%s\n",
					tag, i, fmt, fmt, tymed, typeName, fmtName);
				InsertCharacter(std::string_view(buf, len), CharacterSource::TentativeInput);
			}
		}
	}
	if (fmtEnum) {
		fmtEnum->Release();
	}
}

// https://docs.microsoft.com/en-us/windows/desktop/api/winuser/nf-winuser-enumclipboardformats
void ScintillaWin::EnumAllClipboardFormat(const char *tag) {
	UINT fmt = 0;
	unsigned int i = 0;
	char name[1024];
	char buf[2048];
	while ((fmt = ::EnumClipboardFormats(fmt)) != 0) {
		const char *fmtName = GetSourceFormatName(fmt, name, sizeof(name));
		const int len = sprintf(buf, "%s: fmt[%u]=%u, 0x%04X; name=%s\n",
			tag, i, fmt, fmt, fmtName);
		InsertCharacter(std::string_view(buf, len), CharacterSource::TentativeInput);
		i++;
	}
}

#endif

/// Implement IDropTarget
STDMETHODIMP ScintillaWin::DragEnter(LPDATAOBJECT pIDataSource, DWORD grfKeyState, POINTL, PDWORD pdwEffect) {
	if (!pIDataSource) {
		return E_POINTER;
	}

	hasOKText = false;
	try {
		//EnumDataSourceFormat("DragEnter", pIDataSource);

		for (UINT fmtIndex = 0; fmtIndex < dropFormatCount; fmtIndex++) {
			const CLIPFORMAT fmt = dropFormat[fmtIndex];
			FORMATETC fmtu = { fmt, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
			const HRESULT hrHasUText = pIDataSource->QueryGetData(&fmtu);
			hasOKText = (hrHasUText == S_OK);
			if (hasOKText) {
				break;
			}
		}
	} catch (...) {
		errorStatus = Status::Failure;
	}

	*pdwEffect = hasOKText? EffectFromState(grfKeyState) : DROPEFFECT_NONE;
	return S_OK;
}

STDMETHODIMP ScintillaWin::DragOver(DWORD grfKeyState, POINTL pt, PDWORD pdwEffect) {
	try {
		if (!hasOKText || pdoc->IsReadOnly()) {
			*pdwEffect = DROPEFFECT_NONE;
			return S_OK;
		}

		*pdwEffect = EffectFromState(grfKeyState);

		// Update the cursor.
		POINT rpt = { pt.x, pt.y };
		::ScreenToClient(MainHWND(), &rpt);
		SetDragPosition(SPositionFromLocation(PointFromPOINT(rpt), false, false, UserVirtualSpace()));

		return S_OK;
	} catch (...) {
		errorStatus = Status::Failure;
	}
	return E_FAIL;
}

STDMETHODIMP ScintillaWin::DragLeave() {
	try {
		SetDragPosition(SelectionPosition(Sci::invalidPosition));
		return S_OK;
	} catch (...) {
		errorStatus = Status::Failure;
	}
	return E_FAIL;
}

STDMETHODIMP ScintillaWin::Drop(LPDATAOBJECT pIDataSource, DWORD grfKeyState, POINTL pt, PDWORD pdwEffect) {
	try {
		*pdwEffect = EffectFromState(grfKeyState);

		if (!pIDataSource) {
			return E_POINTER;
		}

		SetDragPosition(SelectionPosition(Sci::invalidPosition));

		std::string putf;
		HRESULT hr = DV_E_FORMATETC;

		//EnumDataSourceFormat("Drop", pIDataSource);
		for (UINT fmtIndex = 0; fmtIndex < dropFormatCount; fmtIndex++) {
			const CLIPFORMAT fmt = dropFormat[fmtIndex];
			FORMATETC fmtu = { fmt, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
			STGMEDIUM medium {};
			hr = pIDataSource->GetData(&fmtu, &medium);

			if (SUCCEEDED(hr) && medium.hGlobal) {
				// File Drop
				if (fmt == CF_HDROP
#if EnableDrop_VisualStudioProjectItem
					|| fmt == cfVSStgProjectItem || fmt == cfVSRefProjectItem
#endif
					) {
					HDROP hDrop = static_cast<HDROP>(medium.hGlobal);
					::SendMessage(::GetParent(MainHWND()), APPM_DROPFILES, AsInteger<WPARAM>(hDrop), 0);
				}
#if Enable_ChromiumWebCustomMIMEDataFormat
				else if (fmt == cfChromiumCustomMIME) {
					GlobalMemory memUDrop(medium.hGlobal);
					if (const wchar_t *uptr = static_cast<const wchar_t *>(memUDrop.ptr)) {
						const std::wstring_view wsv(uptr, memUDrop.Size() / 2);
						// parse file url: "resource":"file:///path"
						const size_t dataLen = UTF8Length(wsv);
					}
					memUDrop.Unlock();
				}
#endif
				// Unicode Text
				else if (fmt == CF_UNICODETEXT) {
					GlobalMemory memUDrop(medium.hGlobal);
					if (const wchar_t *uptr = static_cast<const wchar_t *>(memUDrop.ptr)) {
						putf = EncodeWString(uptr);
					}
					memUDrop.Unlock();
				}
				// ANSI Text
				else if (fmt == CF_TEXT) {
					GlobalMemory memDrop(medium.hGlobal);
					if (const char *ptr = static_cast<const char *>(memDrop.ptr)) {
						const std::string_view sv(ptr, strnlen(ptr, memDrop.Size()));
						// In Unicode mode, convert text to UTF-8
						if (IsUnicodeMode()) {
							const std::wstring wsv = StringDecode(sv, CP_ACP);
							putf = StringEncode(wsv, CP_UTF8);
						} else {
							// no need to convert ptr from CP_ACP to CodePageOfDocument().
							putf = sv;
						}
					}
					memDrop.Unlock();
				}
			}

			::ReleaseStgMedium(&medium);
			if (!putf.empty()) {
				break;
			}
		}

		if (!SUCCEEDED(hr)) {
			return hr;
		}
		if (putf.empty()) {
			return S_OK;
		}

		{
			FORMATETC fmtr = { cfColumnSelect, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
			const bool isRectangular = S_OK == pIDataSource->QueryGetData(&fmtr);

			POINT rpt = { pt.x, pt.y };
			::ScreenToClient(MainHWND(), &rpt);
			const SelectionPosition movePos = SPositionFromLocation(PointFromPOINT(rpt), false, false, UserVirtualSpace());

			DropAt(movePos, putf.c_str(), putf.size(), *pdwEffect == DROPEFFECT_MOVE, isRectangular);
		}

		return S_OK;
	} catch (...) {
		errorStatus = Status::Failure;
	}
	return E_FAIL;
}

/// Implement important part of IDataObject
STDMETHODIMP ScintillaWin::GetData(const FORMATETC *pFEIn, STGMEDIUM *pSTM) {
	if (!SupportedFormat(pFEIn)) {
		//Platform::DebugPrintf("DOB GetData No %d %x %x fmt=%x\n", lenDrag, pFEIn, pSTM, pFEIn->cfFormat);
		return DATA_E_FORMATETC;
	}

	pSTM->tymed = TYMED_HGLOBAL;
	//Platform::DebugPrintf("DOB GetData OK %d %x %x\n", lenDrag, pFEIn, pSTM);

	GlobalMemory uniText;
	CopyToGlobal(uniText, drag, (pFEIn->cfFormat == CF_TEXT)? CopyEncoding::Ansi : CopyEncoding::Unicode);
	pSTM->hGlobal = uniText ? uniText.Unlock() : HGLOBAL{};
	pSTM->pUnkForRelease = nullptr;
	return S_OK;
}

#if USE_WIN32_INIT_ONCE
BOOL CALLBACK ScintillaWin::PrepareOnce([[maybe_unused]] PINIT_ONCE initOnce, [[maybe_unused]] PVOID parameter, [[maybe_unused]] PVOID *lpContext) noexcept
#else
void ScintillaWin::PrepareOnce() noexcept
#endif
{
	Platform_Initialise(hInstance);
	CharClassify::InitUnicodeData();

	// Register the CallTip class
	WNDCLASSEX wndclassc {};
	wndclassc.cbSize = sizeof(wndclassc);
	wndclassc.style = CS_GLOBALCLASS | CS_HREDRAW | CS_VREDRAW;
	wndclassc.cbWndExtra = sizeof(LONG_PTR);
	wndclassc.hInstance = hInstance;
	wndclassc.lpfnWndProc = ScintillaWin::CTWndProc;
	wndclassc.hCursor = ::LoadCursor({}, IDC_ARROW);
	wndclassc.lpszClassName = callClassName;

	callClassAtom = ::RegisterClassEx(&wndclassc);
#if USE_WIN32_INIT_ONCE
	return TRUE;
#endif
}

bool ScintillaWin::Register(HINSTANCE hInstance_) noexcept {
	hInstance = hInstance_;

	// Register the Scintilla class
	// Register Scintilla as a wide character window
	WNDCLASSEXW wndclass {};
	wndclass.cbSize = sizeof(wndclass);
	wndclass.style = CS_GLOBALCLASS | CS_HREDRAW | CS_VREDRAW;
	wndclass.lpfnWndProc = ScintillaWin::SWndProc;
	wndclass.cbWndExtra = sizeof(LONG_PTR);
	wndclass.hInstance = hInstance;
	wndclass.lpszClassName = L"Scintilla";
	scintillaClassAtom = ::RegisterClassExW(&wndclass);

	const bool result = 0 != scintillaClassAtom;
	return result;
}

bool ScintillaWin::Unregister() noexcept {
	bool result = true;
	if (0 != scintillaClassAtom) {
		if (::UnregisterClass(MAKEINTATOM(scintillaClassAtom), hInstance) == 0) {
			result = false;
		}
		scintillaClassAtom = 0;
	}
	if (0 != callClassAtom) {
		if (::UnregisterClass(MAKEINTATOM(callClassAtom), hInstance) == 0) {
			result = false;
		}
		callClassAtom = 0;
	}
	return result;
}

bool ScintillaWin::HasCaretSizeChanged() const noexcept {
	return
		((0 != vs.caret.width) && (sysCaretWidth != vs.caret.width))
		|| ((0 != vs.lineHeight) && (sysCaretHeight != vs.lineHeight));
}

BOOL ScintillaWin::CreateSystemCaret() {
	sysCaretWidth = vs.caret.width;
	if (0 == sysCaretWidth) {
		sysCaretWidth = 1;
	}
	sysCaretHeight = vs.lineHeight;
	const int bitmapSize = (((sysCaretWidth + 15) & ~15) >> 3) *
		sysCaretHeight;
	std::vector<BYTE> bits(bitmapSize);
	sysCaretBitmap = ::CreateBitmap(sysCaretWidth, sysCaretHeight, 1,
		1, bits.data());
	const BOOL retval = ::CreateCaret(
		MainHWND(), sysCaretBitmap,
		sysCaretWidth, sysCaretHeight);
	if (technology == Technology::Default) {
		// System caret interferes with Direct2D drawing so only show it for GDI.
		::ShowCaret(MainHWND());
	}
	return retval;
}

BOOL ScintillaWin::DestroySystemCaret() noexcept {
	::HideCaret(MainHWND());
	const BOOL retval = ::DestroyCaret();
	if (sysCaretBitmap) {
		::DeleteObject(sysCaretBitmap);
		sysCaretBitmap = {};
	}
	return retval;
}

void ScintillaWin::CTPaint(HWND hWnd) {
	const Painter painter(hWnd);
	const std::unique_ptr<Surface> surfaceWindow(Surface::Allocate(technology));
	HwndRenderTarget pCTRenderTarget;
	if (technology == Technology::Default) {
		surfaceWindow->Init(painter.ps.hdc, hWnd);
	} else {
		const RECT rc = GetClientRect(hWnd);
		// Create a Direct2D render target.

		const D2D1_RENDER_TARGET_PROPERTIES drtp = D2D1::RenderTargetProperties(
			D2D1_RENDER_TARGET_TYPE_DEFAULT,
			{ DXGI_FORMAT_UNKNOWN, D2D1_ALPHA_MODE_UNKNOWN },
			dpiDefault, dpiDefault);

		const D2D1_PRESENT_OPTIONS presentOptions = (technology == Technology::DirectWriteRetain) ?
			D2D1_PRESENT_OPTIONS_RETAIN_CONTENTS : D2D1_PRESENT_OPTIONS_NONE;

		const D2D1_HWND_RENDER_TARGET_PROPERTIES dhrtp = D2D1::HwndRenderTargetProperties(
			hWnd,
			GetSizeUFromRect(rc),
			presentOptions);

		const HRESULT hr = CreateHwndRenderTarget(&drtp, &dhrtp, pCTRenderTarget);
		if (!SUCCEEDED(hr)) {
			surfaceWindow->Release();
			return;
		}
		// If above SUCCEEDED, then pCTRenderTarget not nullptr
		assert(pCTRenderTarget);
		if (pCTRenderTarget) {
			surfaceWindow->Init(pCTRenderTarget.Get(), hWnd);
			SetRenderingParams(surfaceWindow.get());
			pCTRenderTarget->BeginDraw();
		}
	}
	surfaceWindow->SetMode(CurrentSurfaceMode());
	ct.PaintCT(surfaceWindow.get());
	if (pCTRenderTarget) {
		pCTRenderTarget->EndDraw();
	}
	surfaceWindow->Release();
}

LRESULT ScintillaWin::CTProcessMessage(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam) {
	try {
		switch (iMessage) {
		case WM_NCDESTROY:
			SetWindowPointer(hWnd, nullptr);
			break;
		case WM_PAINT:
			CTPaint(hWnd);
			return 0;
		case WM_NCLBUTTONDOWN:
		case WM_NCLBUTTONDBLCLK: {
				POINT pt = POINTFromLParam(lParam);
				::ScreenToClient(hWnd, &pt);
				ct.MouseClick(PointFromPOINTEx(pt));
				CallTipClick();
				return 0;
			}
		case WM_LBUTTONDOWN:
			// This does not fire due to the hit test code
			ct.MouseClick(PointFromLParam(lParam));
			CallTipClick();
			return 0;
		case WM_SETCURSOR:
			::SetCursor(::LoadCursor({}, IDC_ARROW));
			return 0;
		case WM_NCHITTEST:
			return HTCAPTION;
		default:
			break;
		}
	} catch (...) {
		errorStatus = Status::Failure;
	}

	return ::DefWindowProc(hWnd, iMessage, wParam, lParam);
}

LRESULT CALLBACK ScintillaWin::CTWndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam) {
	// Find C++ object associated with window.
	ScintillaWin *sciThis = PointerFromWindow<ScintillaWin *>(hWnd);
	// sciThis will be zero if WM_CREATE not seen yet
	if (sciThis == nullptr) {
		if (iMessage == WM_CREATE) {
			// Associate CallTip object with window
			const CREATESTRUCT *pCreate = AsPointer<CREATESTRUCT *>(lParam);
			SetWindowPointer(hWnd, pCreate->lpCreateParams);
			return 0;
		}
		return ::DefWindowProc(hWnd, iMessage, wParam, lParam);
	}

	return sciThis->CTProcessMessage(hWnd, iMessage, wParam, lParam);
}

//sptr_t ScintillaWin::DirectFunction(sptr_t ptr, UINT iMessage, uptr_t wParam, sptr_t lParam) {
//	ScintillaWin *sci = AsPointer<ScintillaWin *>(ptr);
//	PLATFORM_ASSERT(::GetCurrentThreadId() == ::GetWindowThreadProcessId(sci->MainHWND(), nullptr));
//	return sci->WndProc(static_cast<Message>(iMessage), wParam, lParam);
//}
//
//sptr_t ScintillaWin::DirectStatusFunction(sptr_t ptr, UINT iMessage, uptr_t wParam, sptr_t lParam, int *pStatus) {
//	ScintillaWin *sci = AsPointer<ScintillaWin *>(ptr);
//	PLATFORM_ASSERT(::GetCurrentThreadId() == ::GetWindowThreadProcessId(sci->MainHWND(), nullptr));
//	const sptr_t returnValue = sci->WndProc(static_cast<Message>(iMessage), wParam, lParam);
//	*pStatus = static_cast<int>(sci->errorStatus);
//	return returnValue;
//}

extern "C"
LRESULT SCI_METHOD Scintilla_DirectFunction(ScintillaWin *sci, UINT iMessage, LPARAM wParam, WPARAM lParam) {
	return sci->WndProc(static_cast<Message>(iMessage), wParam, lParam);
}

LRESULT CALLBACK ScintillaWin::SWndProc(HWND hWnd, UINT iMessage, WPARAM wParam, LPARAM lParam) {
	//Platform::DebugPrintf("S W:%x M:%x WP:%x L:%x\n", hWnd, iMessage, wParam, lParam);

	// Find C++ object associated with window.
	ScintillaWin *sci = PointerFromWindow<ScintillaWin *>(hWnd);
	// sci will be zero if WM_CREATE not seen yet
	if (sci == nullptr) {
		try {
			if (iMessage == WM_CREATE) {
#if USE_STD_CALL_ONCE
				static std::once_flag once;
				std::call_once(once, PrepareOnce);
#elif USE_WIN32_INIT_ONCE
				static INIT_ONCE once = INIT_ONCE_STATIC_INIT;
				::InitOnceExecuteOnce(&once, PrepareOnce, nullptr, nullptr);
#else
				static LONG once = 0;
				if (::InterlockedCompareExchange(&once, 1, 0) == 0) {
					PrepareOnce();
				}
#endif
				// Create C++ object associated with window
				sci = new ScintillaWin(hWnd);
				SetWindowPointer(hWnd, sci);
				return sci->WndProc(static_cast<Message>(iMessage), wParam, lParam);
			}
		} catch (...) {
		}
		return ::DefWindowProc(hWnd, iMessage, wParam, lParam);
	} else {
		if (iMessage == WM_NCDESTROY) {
			try {
				sci->Finalise();
				delete sci;
			} catch (...) {
			}
			SetWindowPointer(hWnd, nullptr);
			return ::DefWindowProc(hWnd, iMessage, wParam, lParam);
		} else {
			return sci->WndProc(static_cast<Message>(iMessage), wParam, lParam);
		}
	}
}

extern "C" {

// This function is externally visible so it can be called from container when building statically.
// Must be called once only.
int Scintilla_RegisterClasses(void *hInstance) {
	const bool result = ScintillaWin::Register(static_cast<HINSTANCE>(hInstance));
	return result;
}

// This function is externally visible so it can be called from container when building statically.
int Scintilla_ReleaseResources(void) {
	const bool result = ScintillaWin::Unregister();
	Platform_Finalise(false);
	return result;
}

}
