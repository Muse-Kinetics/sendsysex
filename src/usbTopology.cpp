#include "usbTopology.h"

#include <algorithm>
#include <cctype>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <initguid.h> // before devpkey.h, so the DEVPKEY_* are defined here
#include <devpkey.h>
#include <cfgmgr32.h>
#endif

namespace usbTopology
{
namespace
{
std::string squash(const std::string &s)
{
    std::string out;
    for (unsigned char c : s)
        if (!std::isspace(c))
            out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

#if defined(_WIN32)
std::string narrow(const std::wstring &w)
{
    if (w.empty())
        return std::string();
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, 0, 0, 0, 0);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    if (n > 1)
        WideCharToMultiByte(CP_UTF8, 0, w.c_str(), -1, &s[0], n, 0, 0);
    return s;
}

std::wstring instanceId(DEVINST inst)
{
    WCHAR buf[MAX_DEVICE_ID_LEN + 1] = {0};
    return CM_Get_Device_IDW(inst, buf, MAX_DEVICE_ID_LEN, 0) == CR_SUCCESS ? buf : L"";
}

std::wstring stringProperty(DEVINST inst, const DEVPROPKEY &key)
{
    DEVPROPTYPE type = 0;
    WCHAR buf[512] = {0};
    ULONG size = sizeof(buf);
    if (CM_Get_DevNode_PropertyW(inst, &key, &type, reinterpret_cast<PBYTE>(buf), &size, 0) != CR_SUCCESS ||
        type != DEVPROP_TYPE_STRING)
        return L"";
    return buf;
}

bool startsWith(const std::wstring &s, const wchar_t *prefix)
{
    return _wcsnicmp(s.c_str(), prefix, wcslen(prefix)) == 0;
}

// "USB\VID_1A40&PID_0101\..." -> "1A40:0101", or "" if not in that form.
std::string vidPid(const std::wstring &id)
{
    const std::size_t v = id.find(L"VID_"), p = id.find(L"PID_");
    if (v == std::wstring::npos || p == std::wstring::npos)
        return std::string();
    return narrow(id.substr(v + 4, 4)) + ":" + narrow(id.substr(p + 4, 4));
}

// QEMU/KVM, VMware, VirtualBox and Hyper-V USB controllers.
bool isVirtualController(const std::wstring &id)
{
    static const wchar_t *vendors[] = {L"VEN_1B36", L"VEN_1033&DEV_0194&SUBSYS_11001AF4", L"VEN_15AD",
                                       L"VEN_80EE", L"VMBUS"};
    for (const wchar_t *v : vendors)
        if (id.find(v) != std::wstring::npos)
            return true;
    return false;
}
#endif
}

std::vector<Connection> find(const std::vector<std::string> &productStrings)
{
    std::vector<Connection> found;
#if defined(_WIN32)
    const ULONG flags = CM_GETIDLIST_FILTER_ENUMERATOR | CM_GETIDLIST_FILTER_PRESENT;
    ULONG len = 0;
    if (CM_Get_Device_ID_List_SizeW(&len, L"USB", flags) != CR_SUCCESS || len == 0)
        return found;
    std::vector<WCHAR> list(len);
    if (CM_Get_Device_ID_ListW(L"USB", list.data(), len, flags) != CR_SUCCESS)
        return found;

    std::vector<std::string> wanted;
    for (const std::string &p : productStrings)
        wanted.push_back(squash(p));

    for (const WCHAR *id = list.data(); *id; id += wcslen(id) + 1)
    {
        const std::wstring devId(id);
        if (devId.find(L"&MI_") != std::wstring::npos || startsWith(devId, L"USB\\ROOT_HUB"))
            continue; // interfaces of a composite device, and root hubs
        DEVINST inst = 0;
        if (CM_Locate_DevNodeW(&inst, const_cast<DEVINSTID_W>(id), CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS)
            continue;
        const std::string product = narrow(stringProperty(inst, DEVPKEY_Device_BusReportedDeviceDesc));
        if (std::find(wanted.begin(), wanted.end(), squash(product)) == wanted.end())
            continue;

        Connection c;
        c.product = product;
        c.instanceId = narrow(devId);
        DEVINST node = inst, parent = 0;
        while (CM_Get_Parent(&parent, node, 0) == CR_SUCCESS)
        {
            const std::wstring pid = instanceId(parent);
            if (startsWith(pid, L"USB\\ROOT_HUB"))
            {
                DEVINST controller = 0;
                if (CM_Get_Parent(&controller, parent, 0) == CR_SUCCESS)
                    c.virtualController = isVirtualController(instanceId(controller));
                break;
            }
            if (!startsWith(pid, L"USB\\"))
                break; // left the USB tree without meeting a root hub
            std::wstring name = stringProperty(parent, DEVPKEY_Device_BusReportedDeviceDesc);
            if (name.empty())
                name = stringProperty(parent, DEVPKEY_Device_DeviceDesc);
            const std::string ids = vidPid(pid);
            c.hubs.push_back((name.empty() ? std::string("USB hub") : narrow(name)) +
                             (ids.empty() ? std::string() : " (" + ids + ")"));
            node = parent;
        }
        found.push_back(c);
    }
#else
    (void)productStrings;
#endif
    return found;
}

std::string describe(const Connection &c)
{
    std::string s = "USB connection: \"" + c.product + "\" ";
    if (c.hubs.empty())
        s += "is plugged straight into the computer (root port)";
    else
    {
        s += "is behind " + std::to_string(c.hubs.size()) + (c.hubs.size() == 1 ? " hub: " : " hubs: ");
        for (std::size_t i = 0; i < c.hubs.size(); ++i)
            s += (i ? " -> " : "") + c.hubs[i];
    }
    if (c.virtualController)
        s += " [virtual machine: hubs on the host computer aren't visible here]";
    return s;
}
}
