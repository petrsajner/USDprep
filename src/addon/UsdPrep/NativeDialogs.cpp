#include "NativeDialogs.h"

#ifdef _WIN32

#include <windows.h>
#include <shobjidl.h>

#include <GLFW/glfw3.h>
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>

namespace usdprep_addon {

namespace {

std::string WideToUtf8(const wchar_t* w) {
    if (!w || !*w) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (size <= 1) return {};
    std::string out(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &out[0], size, nullptr, nullptr);
    return out;
}

std::wstring Utf8ToWide(const std::string& s) {
    if (s.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), static_cast<int>(s.size()), &out[0], size);
    return out;
}

bool EnsureCom() {
    static const bool initialized = [] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        return SUCCEEDED(hr) || hr == RPC_E_CHANGED_MODE;
    }();
    return initialized;
}

// The dialog belongs to the program's window: it stays in front of it, and
// the window waits for it instead of looking frozen behind it.
HWND Owner() {
    GLFWwindow* window = glfwGetCurrentContext();
    return window ? glfwGetWin32Window(window) : nullptr;
}

// Each dialog remembers its own folder: the scenes are opened from one
// place, the exports go to another.
constexpr GUID kSceneDialog = {0x6f2b1a57, 0x3c1e, 0x4d8a, {0x9b, 0x61, 0x2e, 0x4c, 0x70, 0x15, 0xa3, 0x01}};
constexpr GUID kSaveDialog = {0x6f2b1a57, 0x3c1e, 0x4d8a, {0x9b, 0x61, 0x2e, 0x4c, 0x70, 0x15, 0xa3, 0x02}};
constexpr GUID kRecipeDialog = {0x6f2b1a57, 0x3c1e, 0x4d8a, {0x9b, 0x61, 0x2e, 0x4c, 0x70, 0x15, 0xa3, 0x03}};

// Runs a prepared dialog to its result path. Empty = cancelled.
std::string DialogResult(IFileDialog* dialog) {
    std::string result;
    DWORD options = 0;
    if (SUCCEEDED(dialog->GetOptions(&options))) dialog->SetOptions(options | FOS_FORCEFILESYSTEM);
    if (SUCCEEDED(dialog->Show(Owner()))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                result = WideToUtf8(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    return result;
}

template <typename Dialog>
Dialog* Create(REFCLSID kind) {
    if (!EnsureCom()) return nullptr;
    Dialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(kind, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return nullptr;
    return dialog;
}

}  // namespace

bool NativeDialogsAvailable() { return true; }

bool NativeSaveDialog(const std::string& suggestedName, int format, std::string& outPath) {
    IFileSaveDialog* dialog = Create<IFileSaveDialog>(CLSID_FileSaveDialog);
    if (!dialog) return false;
    // The formats Nuke was measured to read: USD, and .abc / .obj for its classic 3D.
    const COMDLG_FILTERSPEC filters[] = {{L"USD for Nuke (*.usdc)", L"*.usdc"},
                                         {L"Alembic for older Nuke (*.abc)", L"*.abc"},
                                         {L"OBJ for older Nuke (*.obj)", L"*.obj"}};
    dialog->SetClientGuid(kSaveDialog);
    dialog->SetFileTypes(3, filters);
    dialog->SetFileTypeIndex(static_cast<UINT>(format + 1));
    dialog->SetDefaultExtension(format == 1 ? L"abc" : format == 2 ? L"obj" : L"usdc");
    const std::wstring suggested = Utf8ToWide(suggestedName);
    if (!suggested.empty()) dialog->SetFileName(suggested.c_str());
    const std::string result = DialogResult(dialog);
    dialog->Release();
    if (result.empty()) return false;
    outPath = result;
    return true;
}

bool NativeOpenRecipeDialog(std::string& outPath) {
    IFileOpenDialog* dialog = Create<IFileOpenDialog>(CLSID_FileOpenDialog);
    if (!dialog) return false;
    const COMDLG_FILTERSPEC filters[] = {{L"Recipe (*.json)", L"*.json"}};
    dialog->SetClientGuid(kRecipeDialog);
    dialog->SetFileTypes(1, filters);
    const std::string result = DialogResult(dialog);
    dialog->Release();
    if (result.empty()) return false;
    outPath = result;
    return true;
}

bool NativeOpenSceneDialog(std::string& outPath) {
    IFileOpenDialog* dialog = Create<IFileOpenDialog>(CLSID_FileOpenDialog);
    if (!dialog) return false;
    const COMDLG_FILTERSPEC filters[] = {
        {L"Scenes and models (*.usd, *.usda, *.usdc, *.usdz, *.obj)", L"*.usd;*.usda;*.usdc;*.usdz;*.obj"},
        {L"USD (*.usd, *.usda, *.usdc, *.usdz)", L"*.usd;*.usda;*.usdc;*.usdz"},
        {L"OBJ - converted to USD on opening (*.obj)", L"*.obj"},
        {L"All files (*.*)", L"*.*"}};
    dialog->SetClientGuid(kSceneDialog);
    dialog->SetTitle(L"Open a scene");
    dialog->SetFileTypes(4, filters);
    const std::string result = DialogResult(dialog);
    dialog->Release();
    if (result.empty()) return false;
    outPath = result;
    return true;
}

}  // namespace usdprep_addon

#else  // !_WIN32

namespace usdprep_addon {

bool NativeDialogsAvailable() { return false; }
bool NativeSaveDialog(const std::string&, int, std::string&) { return false; }
bool NativeOpenRecipeDialog(std::string&) { return false; }
bool NativeOpenSceneDialog(std::string&) { return false; }

}  // namespace usdprep_addon

#endif
