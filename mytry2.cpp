// ISS_stream_control.cpp
// Build: cl /EHsc mytry.cpp /link Gdiplus.lib Ws2_32.lib Gdi32.lib User32.lib Ole32.lib
// NOTE: Run vcvars64.bat before compiling for x64 build.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <objidl.h>
#include <gdiplus.h>
#include <comdef.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>

#include <iostream>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <string>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <sstream>

#pragma comment(lib, "Ws2_32.lib")
#pragma comment(lib, "Gdiplus.lib")
#pragma comment(lib, "Ole32.lib")

using namespace Gdiplus;

// Forward declaration
class Client;

// ---------- helpers ----------
int sendAll(SOCKET s, const char* buf, int len) {
    int sent = 0;
    while (sent < len) {
        int r = send(s, buf + sent, len - sent, 0);
        if (r == SOCKET_ERROR) return SOCKET_ERROR;
        if (r == 0) return sent;
        sent += r;
    }
    return sent;
}
int recvAll(SOCKET s, char* buf, int len) {
    int rec = 0;
    while (rec < len) {
        int r = recv(s, buf + rec, len - rec, 0);
        if (r == SOCKET_ERROR) return SOCKET_ERROR;
        if (r == 0) return rec;
        rec += r;
    }
    return rec;
}

// Encode an HBITMAP (region) to JPEG bytes via GDI+
bool EncodeHBITMAPToJPEGBytes(HBITMAP hBmp, RECT srcRect, std::vector<BYTE>& out, ULONG quality=80) {
    if (!hBmp) return false;
    bool ok = false;
    Bitmap bmp(hBmp, NULL);
    if (bmp.GetLastStatus() != Ok) return false;

    // Crop by drawing region into new Bitmap
    int w = srcRect.right - srcRect.left;
    int h = srcRect.bottom - srcRect.top;
    if (w <= 0 || h <= 0) return false;

    Bitmap region(w, h, PixelFormat32bppARGB);
    if (region.GetLastStatus() != Ok) return false;

    {
        Graphics g(&region);
        if (g.GetLastStatus() != Ok) return false;
        g.DrawImage(&bmp, Rect(0,0,w,h), srcRect.left, srcRect.top, w, h, UnitPixel);
        if (g.GetLastStatus() != Ok) return false;
    }

    // find JPEG encoder
    UINT nEnc = 0, sizeEnc = 0;
    GetImageEncodersSize(&nEnc, &sizeEnc);
    if (sizeEnc == 0 || nEnc == 0) return false;
    ImageCodecInfo* pInfo = (ImageCodecInfo*)malloc(sizeEnc);
    if (!pInfo) return false;
    GetImageEncoders(nEnc, sizeEnc, pInfo);
    CLSID clsidJpeg = {0};
    for (UINT i=0;i<nEnc;++i) {
        if (wcscmp(pInfo[i].MimeType, L"image/jpeg") == 0) { clsidJpeg = pInfo[i].Clsid; break;}
    }
    free(pInfo);
    if (clsidJpeg.Data1 == 0 && clsidJpeg.Data2 == 0) return false;

    IStream* ist = nullptr;
    if (CreateStreamOnHGlobal(NULL, TRUE, &ist) != S_OK) return false;

    EncoderParameters ep;
    ep.Count = 1;
    EncoderParameter e;
    e.Guid = EncoderQuality;
    e.NumberOfValues = 1;
    e.Type = EncoderParameterValueTypeLong;
    e.Value = &quality;
    ep.Parameter[0] = e;

    if (region.Save(ist, &clsidJpeg, &ep) == Ok) {
        HGLOBAL hg = NULL;
        if (GetHGlobalFromStream(ist, &hg) == S_OK && hg) {
            SIZE_T sz = GlobalSize(hg);
            if (sz > 0) {
                void* p = GlobalLock(hg);
                if (p) {
                    out.assign((BYTE*)p, (BYTE*)p + sz);
                    GlobalUnlock(hg);
                    ok = true;
                }
            }
        }
    }
    if (ist) ist->Release();
    return ok;
}

// Decode JPEG bytes to HBITMAP
HBITMAP DecodeJPEGBytesToHBITMAP(const BYTE* data, size_t len) {
    if (!data || len == 0) return NULL;
    HBITMAP hBmp = NULL;
    HGLOBAL hGlobal = GlobalAlloc(GMEM_MOVEABLE, len);
    if (!hGlobal) return NULL;
    void* mem = GlobalLock(hGlobal);
    if (!mem) { GlobalFree(hGlobal); return NULL; }
    memcpy(mem, data, len);
    GlobalUnlock(hGlobal);
    IStream* ist = nullptr;
    if (CreateStreamOnHGlobal(hGlobal, TRUE, &ist) != S_OK) { GlobalFree(hGlobal); return NULL; }
    Bitmap* b = Bitmap::FromStream(ist, FALSE);
    if (ist) ist->Release();
    if (!b || b->GetLastStatus() != Ok) { delete b; GlobalFree(hGlobal); return NULL; }
    b->GetHBITMAP(Color(0,0,0), &hBmp);
    delete b;
    GlobalFree(hGlobal);
    return hBmp;
}

// Window selection dialog
HWND g_selectedWindow = NULL;
BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam) {
    if (!IsWindowVisible(hwnd)) return TRUE;

    char title[256];
    GetWindowTextA(hwnd, title, sizeof(title));
    if (strlen(title) == 0) return TRUE;

    std::vector<std::string>* windows = (std::vector<std::string>*)lParam;
    std::stringstream ss;
    ss << (void*)hwnd << "|" << title;
    windows->push_back(ss.str());
    return TRUE;
}

HWND SelectWindowToCapture() {
    std::vector<std::string> windows;
    EnumWindows(EnumWindowsProc, (LPARAM)&windows);

    std::cout << "\nSelect what to capture:\n";
    std::cout << "0. Full Screen (default)\n";
    for (size_t i = 0; i < windows.size() && i < 20; ++i) {
        size_t pos = windows[i].find('|');
        std::string title = windows[i].substr(pos + 1);
        std::cout << (i + 1) << ". " << title << "\n";
    }

    std::cout << "\nEnter choice (0-" << min(windows.size(), (size_t)20) << "): ";
    std::string input;
    std::getline(std::cin, input);

    int choice = 0;
    if (!input.empty()) choice = atoi(input.c_str());

    if (choice == 0 || choice > (int)windows.size()) {
        std::cout << "Capturing full screen\n";
        return NULL;
    }

    size_t pos = windows[choice - 1].find('|');
    std::string hwndStr = windows[choice - 1].substr(0, pos);
    HWND hwnd = (HWND)strtoull(hwndStr.c_str(), NULL, 16);

    std::cout << "Capturing window: " << windows[choice - 1].substr(pos + 1) << "\n";
    return hwnd;
}

// ---------- Audio Capture ----------
class AudioCapture {
public:
    AudioCapture() : m_enumerator(nullptr), m_device(nullptr), m_client(nullptr), m_capture(nullptr), m_running(false) {}
    ~AudioCapture() { stop(); }

private:
    IMMDeviceEnumerator* m_enumerator;
    IMMDevice* m_device;
    IAudioClient* m_client;
    IAudioCaptureClient* m_capture;
    std::atomic<bool> m_running;
    std::thread m_thread;
    SOCKET m_audioSocket;

    void captureLoop() {
        WAVEFORMATEX* pwfx = nullptr;
        m_client->GetMixFormat(&pwfx);
        UINT32 bytesPerFrame = pwfx ? pwfx->nBlockAlign : 4;
        if (pwfx) CoTaskMemFree(pwfx);

        while (m_running) {
            UINT32 packetLength = 0;
            HRESULT hr = m_capture->GetNextPacketSize(&packetLength);
            if (FAILED(hr)) {
                std::cerr << "Audio capture GetNextPacketSize failed\n";
                break;
            }

            while (packetLength != 0 && m_running) {
                BYTE* pData;
                UINT32 numFramesAvailable;
                DWORD flags;

                hr = m_capture->GetBuffer(&pData, &numFramesAvailable, &flags, NULL, NULL);
                if (FAILED(hr)) break;

                if (!(flags & AUDCLNT_BUFFERFLAGS_SILENT) && numFramesAvailable > 0) {
                    uint32_t size = numFramesAvailable * bytesPerFrame;
                    if (sendAll(m_audioSocket, (char*)&size, 4) == SOCKET_ERROR) {
                        m_capture->ReleaseBuffer(numFramesAvailable);
                        break;
                    }
                    if (sendAll(m_audioSocket, (char*)pData, size) == SOCKET_ERROR) {
                        m_capture->ReleaseBuffer(numFramesAvailable);
                        break;
                    }
                }

                hr = m_capture->ReleaseBuffer(numFramesAvailable);
                if (FAILED(hr)) break;

                hr = m_capture->GetNextPacketSize(&packetLength);
                if (FAILED(hr)) break;
            }

            Sleep(10);
        }
        std::cout << "Audio capture loop ended\n";
    }

public:

    bool start(SOCKET audioSocket) {
        m_audioSocket = audioSocket;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)&m_enumerator);
        if (FAILED(hr)) return false;

        hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
        if (FAILED(hr)) return false;

        hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&m_client);
        if (FAILED(hr)) return false;

        WAVEFORMATEX* pwfx = nullptr;
        hr = m_client->GetMixFormat(&pwfx);
        if (FAILED(hr)) return false;

        hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
            10000000, 0, pwfx, NULL);
        CoTaskMemFree(pwfx);
        if (FAILED(hr)) return false;

        hr = m_client->GetService(__uuidof(IAudioCaptureClient), (void**)&m_capture);
        if (FAILED(hr)) return false;

        hr = m_client->Start();
        if (FAILED(hr)) return false;

        m_running = true;
        m_thread = std::thread(&AudioCapture::captureLoop, this);
        return true;
    }

    void stop() {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
        if (m_client) m_client->Stop();
        if (m_capture) { m_capture->Release(); m_capture = nullptr; }
        if (m_client) { m_client->Release(); m_client = nullptr; }
        if (m_device) { m_device->Release(); m_device = nullptr; }
        if (m_enumerator) { m_enumerator->Release(); m_enumerator = nullptr; }
    }
};

// ---------- server (stream + control) ----------
class Server {
public:
    Server(int portVideo=9632, int portControl=9633, int portWeb=8080, int portAudio=9634) :
        m_portVideo(portVideo), m_portControl(portControl), m_portWeb(portWeb), m_portAudio(portAudio),
        m_listenVideo(INVALID_SOCKET), m_listenControl(INVALID_SOCKET), m_listenWeb(INVALID_SOCKET), m_listenAudio(INVALID_SOCKET),
        m_clientVideo(INVALID_SOCKET), m_clientControl(INVALID_SOCKET), m_clientAudio(INVALID_SOCKET),
        m_running(false), m_captureWindow(NULL) {}

    bool start() {
        // Ask user what to capture
        m_captureWindow = SelectWindowToCapture();

        WSADATA w;
        if (WSAStartup(MAKEWORD(2,2), &w) != 0) { std::cerr<<"WSAStartup failed\n"; return false; }

        // video listen socket
        m_listenVideo = socket(AF_INET, SOCK_STREAM, 0);
        if (m_listenVideo == INVALID_SOCKET) return false;
        sockaddr_in srv{};
        srv.sin_family = AF_INET; srv.sin_addr.s_addr = INADDR_ANY; srv.sin_port = htons(m_portVideo);
        int opt = 1; setsockopt(m_listenVideo, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
        if (bind(m_listenVideo, (sockaddr*)&srv, sizeof(srv)) == SOCKET_ERROR) { closesocket(m_listenVideo); return false; }
        if (listen(m_listenVideo, 1) == SOCKET_ERROR) { closesocket(m_listenVideo); return false; }

        // control listen
        m_listenControl = socket(AF_INET, SOCK_STREAM, 0);
        if (m_listenControl == INVALID_SOCKET) return false;
        sockaddr_in srv2 = srv; srv2.sin_port = htons(m_portControl);
        setsockopt(m_listenControl, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
        if (bind(m_listenControl, (sockaddr*)&srv2, sizeof(srv2)) == SOCKET_ERROR) { closesocket(m_listenControl); return false; }
        if (listen(m_listenControl, 1) == SOCKET_ERROR) { closesocket(m_listenControl); return false; }

        // web server listen
        m_listenWeb = socket(AF_INET, SOCK_STREAM, 0);
        if (m_listenWeb == INVALID_SOCKET) return false;
        sockaddr_in srv3 = srv; srv3.sin_port = htons(m_portWeb);
        setsockopt(m_listenWeb, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
        if (bind(m_listenWeb, (sockaddr*)&srv3, sizeof(srv3)) == SOCKET_ERROR) { closesocket(m_listenWeb); return false; }
        if (listen(m_listenWeb, 5) == SOCKET_ERROR) { closesocket(m_listenWeb); return false; }

        // audio listen
        m_listenAudio = socket(AF_INET, SOCK_STREAM, 0);
        if (m_listenAudio == INVALID_SOCKET) return false;
        sockaddr_in srv4 = srv; srv4.sin_port = htons(m_portAudio);
        setsockopt(m_listenAudio, SOL_SOCKET, SO_REUSEADDR, (char*)&opt, sizeof(opt));
        if (bind(m_listenAudio, (sockaddr*)&srv4, sizeof(srv4)) == SOCKET_ERROR) { closesocket(m_listenAudio); return false; }
        if (listen(m_listenAudio, 1) == SOCKET_ERROR) { closesocket(m_listenAudio); return false; }

        // Get local IP address
        char hostname[256];
        gethostname(hostname, sizeof(hostname));
        hostent* host = gethostbyname(hostname);
        char* ip = inet_ntoa(*(in_addr*)host->h_addr_list[0]);

        std::cout << "Server IP: " << ip << "\n";
        std::cout << "Server listening video:" << m_portVideo << " control:" << m_portControl << " audio:" << m_portAudio << " web:" << m_portWeb << "\n";

        // Start the web server thread immediately
        m_running = true;
        m_threadWeb = std::thread(&Server::webServerLoop, this);

        // Now wait for video, control, and audio clients
        std::cout << "Waiting for video, control, and audio clients...\n";
        sockaddr_in cli; int len = sizeof(cli);
        m_clientVideo = accept(m_listenVideo, (sockaddr*)&cli, &len);
        if (m_clientVideo == INVALID_SOCKET) { std::cerr<<"accept video failed\n"; return false; }
        std::cout<<"Video client connected\n";

        len = sizeof(cli);
        m_clientControl = accept(m_listenControl, (sockaddr*)&cli, &len);
        if (m_clientControl == INVALID_SOCKET) { std::cerr<<"accept control failed\n"; return false; }
        std::cout<<"Control client connected\n";

        len = sizeof(cli);
        m_clientAudio = accept(m_listenAudio, (sockaddr*)&cli, &len);
        if (m_clientAudio == INVALID_SOCKET) { std::cerr<<"accept audio failed\n"; return false; }
        std::cout<<"Audio client connected\n";

        // non-blocking basic timeout
        DWORD timeout = 5000;
        setsockopt(m_clientVideo, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
        setsockopt(m_clientControl, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));
        setsockopt(m_clientAudio, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));

        m_threadCapture = std::thread(&Server::captureLoop, this);
        m_threadControl = std::thread(&Server::controlLoop, this);

        // Start audio capture
        m_audioCapture.start(m_clientAudio);

        return true;
    }

    void stop() {
        m_running = false;
        m_audioCapture.stop();
        if (m_threadCapture.joinable()) m_threadCapture.join();
        if (m_threadControl.joinable()) m_threadControl.join();
        if (m_threadWeb.joinable()) m_threadWeb.join();
        if (m_clientVideo != INVALID_SOCKET) closesocket(m_clientVideo);
        if (m_clientControl != INVALID_SOCKET) closesocket(m_clientControl);
        if (m_clientAudio != INVALID_SOCKET) closesocket(m_clientAudio);
        if (m_listenVideo != INVALID_SOCKET) closesocket(m_listenVideo);
        if (m_listenControl != INVALID_SOCKET) closesocket(m_listenControl);
        if (m_listenWeb != INVALID_SOCKET) closesocket(m_listenWeb);
        if (m_listenAudio != INVALID_SOCKET) closesocket(m_listenAudio);
        WSACleanup();
    }

private:
    int m_portVideo, m_portControl, m_portWeb, m_portAudio;
    SOCKET m_listenVideo, m_listenControl, m_listenWeb, m_listenAudio, m_clientVideo, m_clientControl, m_clientAudio;
    std::atomic<bool> m_running;
    std::thread m_threadCapture, m_threadControl, m_threadWeb;
    std::mutex m_webMutex;
    std::vector<BYTE> m_latestFrame;
    bool m_frameUpdated = false;
    HWND m_captureWindow;
    AudioCapture m_audioCapture;

    const int TILE_W = 256, TILE_H = 256;
    std::unordered_map<uint64_t, uint32_t> prevChecksums;

    void captureLoop() {
        GdiplusStartupInput gdiIn; ULONG_PTR token = 0; 
        if (GdiplusStartup(&token, &gdiIn, NULL) != Ok) {
            std::cerr << "GdiplusStartup failed\n";
            return;
        }

        HDC hScreen = NULL;
        int screenW = 0, screenH = 0;
        int offsetX = 0, offsetY = 0;

        if (m_captureWindow) {
            hScreen = GetDC(m_captureWindow);
            RECT rect;
            GetClientRect(m_captureWindow, &rect);
            screenW = rect.right - rect.left;
            screenH = rect.bottom - rect.top;
        } else {
            hScreen = GetDC(NULL);
            screenW = GetSystemMetrics(SM_CXSCREEN);
            screenH = GetSystemMetrics(SM_CYSCREEN);
        }

        if (!hScreen || screenW <= 0 || screenH <= 0) {
            std::cerr << "Failed to get capture context\n";
            GdiplusShutdown(token);
            return;
        }

        HDC hMem = CreateCompatibleDC(hScreen);
        if (!hMem) {
            std::cerr << "CreateCompatibleDC failed\n";
            ReleaseDC(m_captureWindow ? m_captureWindow : NULL, hScreen);
            GdiplusShutdown(token);
            return;
        }

        HBITMAP hBmp = CreateCompatibleBitmap(hScreen, screenW, screenH);
        if (!hBmp) {
            std::cerr << "CreateCompatibleBitmap failed\n";
            DeleteDC(hMem);
            ReleaseDC(m_captureWindow ? m_captureWindow : NULL, hScreen);
            GdiplusShutdown(token);
            return;
        }

        HGDIOBJ old = SelectObject(hMem, hBmp);
        if (old == HGDI_ERROR) {
            std::cerr << "SelectObject failed\n";
            DeleteObject(hBmp);
            DeleteDC(hMem);
            ReleaseDC(m_captureWindow ? m_captureWindow : NULL, hScreen);
            GdiplusShutdown(token);
            return;
        }

        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = screenW; bi.bmiHeader.biHeight = -screenH;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        std::vector<BYTE> fullBuf(screenW * screenH * 4);

        while (m_running) {
            if (!BitBlt(hMem, 0,0, screenW, screenH, hScreen, offsetX, offsetY, SRCCOPY)) { 
                std::cerr<<"BitBlt failed\n"; 
                break; 
            }

            // overlay cursor only for full screen
            if (!m_captureWindow) {
                CURSORINFO ci{}; ci.cbSize = sizeof(ci);
                if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
                    ICONINFO ii; 
                    if (GetIconInfo(ci.hCursor, &ii)) {
                        DrawIconEx(hMem, ci.ptScreenPos.x - ii.xHotspot, ci.ptScreenPos.y - ii.yHotspot, ci.hCursor, 0,0,0,NULL,DI_NORMAL);
                        if (ii.hbmMask) DeleteObject(ii.hbmMask);
                        if (ii.hbmColor) DeleteObject(ii.hbmColor);
                    }
                }
            }

            if (!GetDIBits(hMem, hBmp, 0, screenH, fullBuf.data(), &bi, DIB_RGB_COLORS)) { 
                std::cerr<<"GetDIBits failed\n"; 
                break; 
            }

            struct Tile { int x,y,w,h; std::vector<BYTE> jpeg; };
            std::vector<Tile> changed;

            for (int ty=0; ty<screenH && m_running; ty+=TILE_H) {
                for (int tx=0; tx<screenW && m_running; tx+=TILE_W) {
                    int w = min(TILE_W, screenW - tx);
                    int h = min(TILE_H, screenH - ty);
                    if (w <= 0 || h <= 0) continue;

                    uint32_t csum = 0;
                    for (int y=0; y<h; ++y) {
                        const uint32_t* row = (const uint32_t*)(fullBuf.data() + (ty+y) * screenW * 4);
                        for (int x=0; x<w; ++x) csum += row[tx + x];
                    }

                    uint64_t key = ((uint64_t)tx << 32) | (uint32_t)ty;
                    uint32_t prev = prevChecksums[key];
                    if (csum != prev) {
                        prevChecksums[key] = csum;

                        HBITMAP tileBmp = CreateCompatibleBitmap(hScreen, w, h);
                        if (!tileBmp) continue;

                        HDC tdc = CreateCompatibleDC(hScreen);
                        if (!tdc) {
                            DeleteObject(tileBmp);
                            continue;
                        }

                        HGDIOBJ oldt = SelectObject(tdc, tileBmp);
                        if (oldt == HGDI_ERROR) {
                            DeleteDC(tdc);
                            DeleteObject(tileBmp);
                            continue;
                        }

                        if (!BitBlt(tdc, 0,0, w,h, hMem, tx, ty, SRCCOPY)) {
                            SelectObject(tdc, oldt);
                            DeleteDC(tdc);
                            DeleteObject(tileBmp);
                            continue;
                        }

                        SelectObject(tdc, oldt);
                        DeleteDC(tdc);

                        RECT rc{0,0,w,h};
                        std::vector<BYTE> jpg;
                        if (!EncodeHBITMAPToJPEGBytes(tileBmp, rc, jpg, 90)) {
                            EncodeHBITMAPToJPEGBytes(tileBmp, rc, jpg, 80);
                        }
                        DeleteObject(tileBmp);
                        if (!jpg.empty()) changed.push_back({tx,ty,w,h, std::move(jpg)});
                    }
                }
            }

            // send if changed
            if (!changed.empty() && m_clientVideo != INVALID_SOCKET && m_running) {
                uint32_t magic = 0x49535332;
                uint32_t w = (uint32_t)screenW, h = (uint32_t)screenH;
                uint32_t tW = TILE_W, tH = TILE_H;
                uint32_t cnt = (uint32_t)changed.size();

                if (sendAll(m_clientVideo, (char*)&magic, 4) == SOCKET_ERROR) break;
                if (sendAll(m_clientVideo, (char*)&w, 4) == SOCKET_ERROR) break;
                if (sendAll(m_clientVideo, (char*)&h, 4) == SOCKET_ERROR) break;
                if (sendAll(m_clientVideo, (char*)&tW, 4) == SOCKET_ERROR) break;
                if (sendAll(m_clientVideo, (char*)&tH, 4) == SOCKET_ERROR) break;
                if (sendAll(m_clientVideo, (char*)&cnt, 4) == SOCKET_ERROR) break;

                for (const auto &t : changed) {
                    uint32_t tx = t.x, ty = t.y, tw = t.w, th = t.h;
                    uint32_t size = (uint32_t)t.jpeg.size();
                    if (sendAll(m_clientVideo, (char*)&tx, 4) == SOCKET_ERROR) break;
                    if (sendAll(m_clientVideo, (char*)&ty, 4) == SOCKET_ERROR) break;
                    if (sendAll(m_clientVideo, (char*)&tw, 4) == SOCKET_ERROR) break;
                    if (sendAll(m_clientVideo, (char*)&th, 4) == SOCKET_ERROR) break;
                    if (sendAll(m_clientVideo, (char*)&size, 4) == SOCKET_ERROR) break;
                    if (sendAll(m_clientVideo, (char*)t.jpeg.data(), size) == SOCKET_ERROR) break;
                }
            }

            // Save full frame for web
            static int frameCounter = 0;
            if (++frameCounter % 5 == 0) {
                HBITMAP fullBmp = CreateCompatibleBitmap(hScreen, screenW, screenH);
                if (fullBmp) {
                    HDC fullDc = CreateCompatibleDC(hScreen);
                    if (fullDc) {
                        HGDIOBJ oldFull = SelectObject(fullDc, fullBmp);
                        if (oldFull != HGDI_ERROR) {
                            BitBlt(fullDc, 0, 0, screenW, screenH, hMem, 0, 0, SRCCOPY);

                            RECT fullRc{0, 0, screenW, screenH};
                            std::vector<BYTE> fullJpeg;
                            if (EncodeHBITMAPToJPEGBytes(fullBmp, fullRc, fullJpeg, 85)) {
                                std::lock_guard<std::mutex> lock(m_webMutex);
                                m_latestFrame = std::move(fullJpeg);
                                m_frameUpdated = true;
                            }
                        }
                        SelectObject(fullDc, oldFull);
                        DeleteDC(fullDc);
                    }
                    DeleteObject(fullBmp);
                }
            }

            if (!m_running) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(40));
        }

        SelectObject(hMem, old);
        DeleteObject(hBmp);
        DeleteDC(hMem);
        ReleaseDC(m_captureWindow ? m_captureWindow : NULL, hScreen);
        if (token) GdiplusShutdown(token);
    }

    void controlLoop() {
        while (m_running) {
            uint8_t type;
            int r = recvAll(m_clientControl, (char*)&type, 1);
            if (r != 1) break;

            if (type == 1) {
                int32_t x,y; 
                if (recvAll(m_clientControl, (char*)&x,4) != 4) break;
                if (recvAll(m_clientControl, (char*)&y,4) != 4) break;
                SetCursorPos(x, y);
            } else if (type == 2 || type == 3) {
                uint8_t btn; int32_t x,y;
                if (recvAll(m_clientControl, (char*)&btn,1) != 1) break;
                if (recvAll(m_clientControl, (char*)&x,4) != 4) break;
                if (recvAll(m_clientControl, (char*)&y,4) != 4) break;

                if (btn != 1 && btn != 2) continue;

                INPUT in[2]; ZeroMemory(in, sizeof(in));
                in[0].type = INPUT_MOUSE;
                in[0].mi.dx = x * (65535 / (GetSystemMetrics(SM_CXSCREEN)-1));
                in[0].mi.dy = y * (65535 / (GetSystemMetrics(SM_CYSCREEN)-1));
                in[0].mi.dwFlags = MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_MOVE;
                SendInput(1, &in[0], sizeof(INPUT));

                ZeroMemory(in, sizeof(in));
                in[0].type = INPUT_MOUSE;
                if (btn == 1) {
                    in[0].mi.dwFlags = (type==2 ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP);
                } else {
                    in[0].mi.dwFlags = (type==2 ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP);
                }
                SendInput(1, &in[0], sizeof(INPUT));
            } else if (type == 4) {
                uint8_t isDown; uint16_t vk;
                if (recvAll(m_clientControl, (char*)&isDown,1) != 1) break;
                if (recvAll(m_clientControl, (char*)&vk,2) != 2) break;

                if (isDown != 0 && isDown != 1) continue;

                INPUT in; ZeroMemory(&in, sizeof(in));
                in.type = INPUT_KEYBOARD;
                in.ki.wVk = vk;
                in.ki.dwFlags = isDown ? 0 : KEYEVENTF_KEYUP;
                SendInput(1, &in, sizeof(INPUT));
            }
        }
        std::cout<<"Control loop ended\n";
    }

    void webServerLoop() {
        while (m_running) {
            sockaddr_in cli;
            int len = sizeof(cli);
            SOCKET client = accept(m_listenWeb, (sockaddr*)&cli, &len);
            if (client == INVALID_SOCKET) {
                Sleep(10);
                continue;
            }

            char buffer[4096];
            int bytes = recv(client, buffer, sizeof(buffer) - 1, 0);
            if (bytes <= 0) {
                closesocket(client);
                continue;
            }
            buffer[bytes] = '\0';

            char* endOfLine = strchr(buffer, '\n');
            if (endOfLine) *(endOfLine + 1) = '\0';

            if (strncmp(buffer, "GET / ", 6) == 0 || strncmp(buffer, "GET / HTTP/1.1", 14) == 0) {
                const char* html = R"(
<!DOCTYPE html>
<html>
<head>
    <title>Screen Share</title>
    <style>
        body { margin: 0; padding: 0; background: #000; }
        img { display: block; width: 100%; height: auto; }
    </style>
</head>
<body>
    <img id="screen" src="/stream" alt="Screen Stream">
</body>
</html>
)";
                std::string response = "HTTP/1.1 200 OK\r\n";
                response += "Content-Type: text/html\r\n";
                response += "Content-Length: " + std::to_string(strlen(html)) + "\r\n";
                response += "Connection: close\r\n";
                response += "\r\n";
                response += html;

                send(client, response.c_str(), response.length(), 0);
            }
            else if (strncmp(buffer, "GET /stream HTTP/1.1", 21) == 0) {
                std::string header = "HTTP/1.1 200 OK\r\n";
                header += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n";
                header += "Connection: close\r\n";
                header += "\r\n";
                send(client, header.c_str(), header.length(), 0);

                while (m_running) {
                    {
                        std::lock_guard<std::mutex> lock(m_webMutex);
                        if (!m_latestFrame.empty()) {
                            std::string frameHeader = "--frame\r\n";
                            frameHeader += "Content-Type: image/jpeg\r\n";
                            frameHeader += "Content-Length: " + std::to_string(m_latestFrame.size()) + "\r\n";
                            frameHeader += "\r\n";

                            int sent = send(client, frameHeader.c_str(), frameHeader.length(), 0);
                            if (sent == SOCKET_ERROR) break;

                            sent = send(client, (const char*)m_latestFrame.data(), m_latestFrame.size(), 0);
                            if (sent == SOCKET_ERROR) break;

                            sent = send(client, "\r\n", 2, 0);
                            if (sent == SOCKET_ERROR) break;
                        }
                    }
                    Sleep(100);
                }
            }
            else {
                const char* notFound = "HTTP/1.1 404 Not Found\r\n\r\n";
                send(client, notFound, strlen(notFound), 0);
            }

            closesocket(client);
        }
    }
};

// ---------- client (receive + send control) ----------
class Client {
public:
    Client(const std::string& ip, int portVideo=9632, int portControl=9633, int portAudio=9634) :
        m_ip(ip), m_portVideo(portVideo), m_portControl(portControl), m_portAudio(portAudio),
        m_sockVideo(INVALID_SOCKET), m_sockControl(INVALID_SOCKET), m_sockAudio(INVALID_SOCKET), m_running(false) {}

    bool start();
    void stop();
    void sendMouseMove(int x, int y);
    void sendMouseButton(uint8_t downOrUp, uint8_t button, int x, int y);
    void sendKey(uint8_t isDown, uint16_t vk);
    bool waitForWindow(int timeoutMs = 10000);
    HWND getWindowHandle();
    int getServerWidth() const { return m_serverWidth; }
    int getServerHeight() const { return m_serverHeight; }

private:
    std::string m_ip; int m_portVideo, m_portControl, m_portAudio;
    SOCKET m_sockVideo, m_sockControl, m_sockAudio;
    std::atomic<bool> m_running;
    std::thread m_threadRecv;
    class ClientWindow* renderWnd = nullptr;
    int m_serverWidth = 0;
    int m_serverHeight = 0;
    class AudioPlayback* m_audioPlayback = nullptr;

    void recvLoop();
};

// ---------- Audio Playback ----------
class AudioPlayback {
public:
    AudioPlayback() : m_device(nullptr), m_client(nullptr), m_render(nullptr), m_running(false) {}
    ~AudioPlayback() { stop(); }

    bool start(SOCKET audioSocket) {
        m_audioSocket = audioSocket;
        HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator), (void**)&m_enumerator);
        if (FAILED(hr)) return false;

        hr = m_enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &m_device);
        if (FAILED(hr)) return false;

        hr = m_device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&m_client);
        if (FAILED(hr)) return false;

        WAVEFORMATEX* pwfx = nullptr;
        hr = m_client->GetMixFormat(&pwfx);
        if (FAILED(hr)) return false;

        hr = m_client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, pwfx, NULL);
        CoTaskMemFree(pwfx);
        if (FAILED(hr)) return false;

        hr = m_client->GetService(__uuidof(IAudioRenderClient), (void**)&m_render);
        if (FAILED(hr)) return false;

        UINT32 bufferFrameCount;
        hr = m_client->GetBufferSize(&bufferFrameCount);
        if (FAILED(hr)) return false;

        hr = m_client->Start();
        if (FAILED(hr)) return false;

        m_running = true;
        m_thread = std::thread(&AudioPlayback::playbackLoop, this);
        return true;
    }

    void stop() {
        m_running = false;
        if (m_thread.joinable()) m_thread.join();
        if (m_client) m_client->Stop();
        if (m_render) { m_render->Release(); m_render = nullptr; }
        if (m_client) { m_client->Release(); m_client = nullptr; }
        if (m_device) { m_device->Release(); m_device = nullptr; }
        if (m_enumerator) { m_enumerator->Release(); m_enumerator = nullptr; }
    }

private:
    IMMDeviceEnumerator* m_enumerator = nullptr;
    IMMDevice* m_device;
    IAudioClient* m_client;
    IAudioRenderClient* m_render;
    std::atomic<bool> m_running;
    std::thread m_thread;
    SOCKET m_audioSocket;

    void playbackLoop() {
        WAVEFORMATEX* pwfx = nullptr;
        m_client->GetMixFormat(&pwfx);
        UINT32 bytesPerFrame = pwfx ? pwfx->nBlockAlign : 4;
        if (pwfx) CoTaskMemFree(pwfx);

        while (m_running) {
            uint32_t size;
            int r = recvAll(m_audioSocket, (char*)&size, 4);
            if (r != 4) {
                std::cerr << "Audio recv failed\n";
                break;
            }
            if (size == 0 || size > 1000000) {
                std::cerr << "Invalid audio size: " << size << "\n";
                break;
            }

            std::vector<BYTE> audioData(size);
            r = recvAll(m_audioSocket, (char*)audioData.data(), size);
            if (r != (int)size) {
                std::cerr << "Audio data recv failed\n";
                break;
            }

            UINT32 numFramesPadding;
            HRESULT hr = m_client->GetCurrentPadding(&numFramesPadding);
            if (FAILED(hr)) continue;

            UINT32 bufferFrameCount;
            hr = m_client->GetBufferSize(&bufferFrameCount);
            if (FAILED(hr)) continue;

            UINT32 numFramesAvailable = bufferFrameCount - numFramesPadding;
            UINT32 numFramesToWrite = size / bytesPerFrame;

            if (numFramesToWrite <= numFramesAvailable) {
                BYTE* pData;
                hr = m_render->GetBuffer(numFramesToWrite, &pData);
                if (SUCCEEDED(hr)) {
                    memcpy(pData, audioData.data(), size);
                    m_render->ReleaseBuffer(numFramesToWrite, 0);
                }
            } else {
                Sleep(10); // Buffer full, wait a bit
            }

            Sleep(5);
        }
    }
};

// Forward declare ClientWindow for Client class
class ClientWindow;

// ---------- client window ----------
class ClientWindow {
public:
    ClientWindow() : hwnd(NULL), dib(NULL), dibPixels(nullptr), width(0), height(0), clientPtr(nullptr) {}
    ~ClientWindow(){ destroy(); }
    bool create(int w, int h, void* client = nullptr) {
        if (w <= 0 || h <= 0) return false;
        width = w; height = h;
        clientPtr = client;

        WNDCLASS wc{}; 
        wc.lpfnWndProc = WndProcStatic; 
        wc.hInstance = GetModuleHandle(NULL); 
        wc.lpszClassName = "ISSClientWndClass";
        wc.hCursor = LoadCursor(NULL, IDC_ARROW);

        if (!GetClassInfo(wc.hInstance, wc.lpszClassName, &wc)) {
            if (!RegisterClass(&wc)) {
                std::cerr << "Failed to register window class\n";
                return false;
            }
        }

        hwnd = CreateWindow(wc.lpszClassName, "ISS Remote View - Click here to control", WS_OVERLAPPEDWINDOW,
                            CW_USEDEFAULT,CW_USEDEFAULT,max(800,w),max(600,h), NULL, NULL, wc.hInstance, this);
        if (!hwnd) {
            std::cerr << "Failed to create window\n";
            return false;
        }

        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);

        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = width; bi.bmiHeader.biHeight = -height; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        HDC dc = GetDC(NULL);
        if (!dc) return false;

        dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &dibPixels, NULL, 0);
        ReleaseDC(NULL, dc);

        if (!dib) return false;

        // Initialize to black
        memset(dibPixels, 0, width * height * 4);

        return true;
    }
    void destroy() { if (dib) { DeleteObject(dib); dib=nullptr; dibPixels=nullptr; } if (hwnd) { DestroyWindow(hwnd); hwnd=NULL; } }
    HWND getHWND() const { return hwnd; }
    void updateTile(int x, int y, HBITMAP tile) {
        if (!dib || !tile || !dibPixels) return;
        BITMAP tb; GetObject(tile, sizeof(tb), &tb);
        int tw = tb.bmWidth, th = tb.bmHeight;

        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = tw; bi.bmiHeader.biHeight = -th; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        std::vector<BYTE> buf(tw * th * 4);
        HDC dc = GetDC(NULL);
        HDC mdc = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mdc, tile);
        GetDIBits(mdc, tile, 0, th, buf.data(), &bi, DIB_RGB_COLORS);
        SelectObject(mdc, old);
        DeleteDC(mdc); ReleaseDC(NULL, dc);

        for (int row=0; row<th && (y+row)<height; ++row) {
            if (x + tw <= width) {
                memcpy((BYTE*)dibPixels + ((y+row) * width + x) * 4, buf.data() + row * tw * 4, tw * 4);
            }
        }
        InvalidateRect(hwnd, NULL, FALSE);
    }
private:
    HWND hwnd; HBITMAP dib; void* dibPixels; int width, height;
    void* clientPtr;

    static LRESULT CALLBACK WndProcStatic(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_CREATE) {
            CREATESTRUCT* cs = (CREATESTRUCT*)lp;
            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return 0;
        }
        ClientWindow* self = (ClientWindow*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (!self) return DefWindowProc(hWnd, msg, wp, lp);

        Client* client = (Client*)self->clientPtr;

        if (msg == WM_PAINT) {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hWnd, &ps);
            if (self->dib) {
                HDC mem = CreateCompatibleDC(dc);
                HGDIOBJ old = SelectObject(mem, self->dib);
                RECT rc; GetClientRect(hWnd, &rc);
                StretchBlt(dc, 0,0, rc.right, rc.bottom, mem, 0,0, self->width, self->height, SRCCOPY);
                SelectObject(mem, old); DeleteDC(mem);
            }
            EndPaint(hWnd, &ps);
            return 0;
        } 
        else if (client && msg == WM_MOUSEMOVE) {
            RECT rc; GetClientRect(hWnd, &rc);
            int mouseX = GET_X_LPARAM(lp);
            int mouseY = GET_Y_LPARAM(lp);
            int serverX = (mouseX * self->width) / rc.right;
            int serverY = (mouseY * self->height) / rc.bottom;
            client->sendMouseMove(serverX, serverY);
            return 0;
        }
        else if (client && msg == WM_LBUTTONDOWN) {
            RECT rc; GetClientRect(hWnd, &rc);
            int mouseX = GET_X_LPARAM(lp);
            int mouseY = GET_Y_LPARAM(lp);
            int serverX = (mouseX * self->width) / rc.right;
            int serverY = (mouseY * self->height) / rc.bottom;
            client->sendMouseButton(2, 1, serverX, serverY);
            return 0;
        }
        else if (client && msg == WM_LBUTTONUP) {
            RECT rc; GetClientRect(hWnd, &rc);
            int mouseX = GET_X_LPARAM(lp);
            int mouseY = GET_Y_LPARAM(lp);
            int serverX = (mouseX * self->width) / rc.right;
            int serverY = (mouseY * self->height) / rc.bottom;
            client->sendMouseButton(3, 1, serverX, serverY);
            return 0;
        }
        else if (client && msg == WM_RBUTTONDOWN) {
            RECT rc; GetClientRect(hWnd, &rc);
            int mouseX = GET_X_LPARAM(lp);
            int mouseY = GET_Y_LPARAM(lp);
            int serverX = (mouseX * self->width) / rc.right;
            int serverY = (mouseY * self->height) / rc.bottom;
            client->sendMouseButton(2, 2, serverX, serverY);
            return 0;
        }
        else if (client && msg == WM_RBUTTONUP) {
            RECT rc; GetClientRect(hWnd, &rc);
            int mouseX = GET_X_LPARAM(lp);
            int mouseY = GET_Y_LPARAM(lp);
            int serverX = (mouseX * self->width) / rc.right;
            int serverY = (mouseY * self->height) / rc.bottom;
            client->sendMouseButton(3, 2, serverX, serverY);
            return 0;
        }
        else if (client && msg == WM_KEYDOWN) {
            client->sendKey(1, (uint16_t)wp);
            return 0;
        }
        else if (client && msg == WM_KEYUP) {
            client->sendKey(0, (uint16_t)wp);
            return 0;
        }
        else if (msg == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(hWnd, msg, wp, lp);
    }
};

// Client implementation
bool Client::start() {
    WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w) != 0) return false;

    m_sockVideo = socket(AF_INET, SOCK_STREAM, 0);
    if (m_sockVideo == INVALID_SOCKET) return false;
    sockaddr_in srv{}; srv.sin_family = AF_INET; srv.sin_port = htons(m_portVideo);
    InetPton(AF_INET, m_ip.c_str(), &srv.sin_addr);
    if (connect(m_sockVideo, (sockaddr*)&srv, sizeof(srv)) == SOCKET_ERROR) { closesocket(m_sockVideo); return false; }

    m_sockControl = socket(AF_INET, SOCK_STREAM, 0);
    if (m_sockControl == INVALID_SOCKET) { closesocket(m_sockVideo); return false; }
    srv.sin_port = htons(m_portControl);
    if (connect(m_sockControl, (sockaddr*)&srv, sizeof(srv)) == SOCKET_ERROR) { closesocket(m_sockVideo); closesocket(m_sockControl); return false; }

    m_sockAudio = socket(AF_INET, SOCK_STREAM, 0);
    if (m_sockAudio == INVALID_SOCKET) { closesocket(m_sockVideo); closesocket(m_sockControl); return false; }
    srv.sin_port = htons(m_portAudio);
    if (connect(m_sockAudio, (sockaddr*)&srv, sizeof(srv)) == SOCKET_ERROR) { closesocket(m_sockVideo); closesocket(m_sockControl); closesocket(m_sockAudio); return false; }

    DWORD tout = 5000;
    setsockopt(m_sockVideo, SOL_SOCKET, SO_RCVTIMEO, (char*)&tout, sizeof(tout));
    setsockopt(m_sockControl, SOL_SOCKET, SO_SNDTIMEO, (char*)&tout, sizeof(tout));
    setsockopt(m_sockAudio, SOL_SOCKET, SO_RCVTIMEO, (char*)&tout, sizeof(tout));

    m_running = true;
    m_threadRecv = std::thread(&Client::recvLoop, this);

    // Start audio playback
    m_audioPlayback = new AudioPlayback();
    m_audioPlayback->start(m_sockAudio);

    return true;
}

void Client::stop() {
    m_running = false;
    if (m_audioPlayback) {
        m_audioPlayback->stop();
        delete m_audioPlayback;
        m_audioPlayback = nullptr;
    }
    if (m_threadRecv.joinable()) m_threadRecv.join();
    if (m_sockVideo != INVALID_SOCKET) closesocket(m_sockVideo);
    if (m_sockControl != INVALID_SOCKET) closesocket(m_sockControl);
    if (m_sockAudio != INVALID_SOCKET) closesocket(m_sockAudio);
    if (renderWnd) renderWnd->destroy();
    WSACleanup();
}

void Client::sendMouseMove(int x, int y) {
    if (m_sockControl == INVALID_SOCKET) return;
    uint8_t t = 1;
    char buf[1+8]; buf[0]=(char)t; memcpy(buf+1,&x,4); memcpy(buf+5,&y,4);
    sendAll(m_sockControl, buf, sizeof(buf));
}

void Client::sendMouseButton(uint8_t downOrUp, uint8_t button, int x, int y) {
    if (m_sockControl == INVALID_SOCKET) return;
    uint8_t t = downOrUp;
    char buf[1+1+8]; buf[0]=(char)t; buf[1]=(char)button; memcpy(buf+2,&x,4); memcpy(buf+6,&y,4);
    sendAll(m_sockControl, buf, sizeof(buf));
}

void Client::sendKey(uint8_t isDown, uint16_t vk) {
    if (m_sockControl == INVALID_SOCKET) return;
    char buf[1+1+2]; buf[0]=4; buf[1]=isDown; memcpy(buf+2,&vk,2);
    sendAll(m_sockControl, buf, sizeof(buf));
}

bool Client::waitForWindow(int timeoutMs) {
    int waited = 0;
    while (!renderWnd || !renderWnd->getHWND() && waited < timeoutMs && m_running) {
        Sleep(100);
        waited += 100;
    }
    return renderWnd != nullptr && renderWnd->getHWND() != NULL;
}

HWND Client::getWindowHandle() {
    if (renderWnd) return renderWnd->getHWND();
    return NULL;
}

void Client::recvLoop() {
    GdiplusStartupInput gi; ULONG_PTR token = 0;
    if (GdiplusStartup(&token, &gi, NULL) != Ok) {
        std::cerr << "Client GdiplusStartup failed\n";
        return;
    }

    renderWnd = new ClientWindow();

    while (m_running) {
        uint32_t magic = 0;
        int r = recvAll(m_sockVideo, (char*)&magic, 4);
        if (r != 4) {
            if (r == SOCKET_ERROR) {
                std::cerr << "Socket error while receiving magic\n";
            } else {
                std::cerr << "Connection closed by server\n";
            }
            break;
        }
        if (magic != 0x49535332) { std::cerr<<"Bad magic\n"; break; }

        uint32_t w,h,tW,tH,count;
        if (recvAll(m_sockVideo, (char*)&w,4) != 4) break;
        if (recvAll(m_sockVideo, (char*)&h,4) != 4) break;
        if (recvAll(m_sockVideo, (char*)&tW,4) != 4) break;
        if (recvAll(m_sockVideo, (char*)&tH,4) != 4) break;
        if (recvAll(m_sockVideo, (char*)&count,4) != 4) break;

        if (w > 10000 || h > 10000 || tW > 1000 || tH > 1000 || count > 10000) {
            std::cerr << "Invalid frame data\n";
            break;
        }

        m_serverWidth = (int)w;
        m_serverHeight = (int)h;

        if (!renderWnd->getHWND()) {
            if (!renderWnd->create((int)w, (int)h, this)) {
                std::cerr << "Failed to create render window\n";
                break;
            }
        }

        for (uint32_t i=0; i<count && m_running; ++i) {
            uint32_t tx,ty,tw,th,sz;
            if (recvAll(m_sockVideo, (char*)&tx,4) != 4) break;
            if (recvAll(m_sockVideo, (char*)&ty,4) != 4) break;
            if (recvAll(m_sockVideo, (char*)&tw,4) != 4) break;
            if (recvAll(m_sockVideo, (char*)&th,4) != 4) break;
            if (recvAll(m_sockVideo, (char*)&sz,4) != 4) break;

            if (tx > w || ty > h || tw > tW || th > tH || sz == 0 || sz > 100*1024*1024) {
                std::cerr << "Invalid tile data\n";
                continue;
            }

            std::vector<BYTE> data(sz);
            if (recvAll(m_sockVideo, (char*)data.data(), (int)sz) != (int)sz) break;

            HBITMAP tile = DecodeJPEGBytesToHBITMAP(data.data(), data.size());
            if (tile) {
                renderWnd->updateTile((int)tx, (int)ty, tile);
                DeleteObject(tile);
            }
        }
    }

    if (token) GdiplusShutdown(token);
    if (renderWnd) delete renderWnd;
}

void printUsage() {
    std::cout << "Usage:\n";
    std::cout << "  Server mode: mytry.exe server [video_port] [control_port] [web_port] [audio_port]\n";
    std::cout << "  Client mode: mytry.exe client <server_ip> [video_port] [control_port] [audio_port]\n";
    std::cout << "  Interactive mode: mytry.exe (no arguments)\n";
}

int getUserChoice() {
    std::cout << "Select mode:\n";
    std::cout << "1. Server\n";
    std::cout << "2. Client\n";
    std::cout << "Enter choice (1 or 2): ";

    int choice;
    std::cin >> choice;
    std::cin.clear();
    std::cin.ignore(10000, '\n');
    return choice;
}

void getServerConfig(int& videoPort, int& controlPort, int& webPort, int& audioPort) {
    std::cout << "Enter video port (default 9632): ";
    std::string input;
    std::getline(std::cin, input);
    if (!input.empty()) videoPort = atoi(input.c_str());

    std::cout << "Enter control port (default 9633): ";
    std::getline(std::cin, input);
    if (!input.empty()) controlPort = atoi(input.c_str());

    std::cout << "Enter web port (default 8080): ";
    std::getline(std::cin, input);
    if (!input.empty()) webPort = atoi(input.c_str());

    std::cout << "Enter audio port (default 9634): ";
    std::getline(std::cin, input);
    if (!input.empty()) audioPort = atoi(input.c_str());

    std::cin.clear();
}

void getClientConfig(std::string& serverIP, int& videoPort, int& controlPort, int& audioPort) {
    std::cout << "Enter server IP (default 127.0.0.1): ";
    std::getline(std::cin, serverIP);
    if (serverIP.empty()) serverIP = "127.0.0.1";

    std::cout << "Enter video port (default 9632): ";
    std::string input;
    std::getline(std::cin, input);
    if (!input.empty()) videoPort = atoi(input.c_str());

    std::cout << "Enter control port (default 9633): ";
    std::getline(std::cin, input);
    if (!input.empty()) controlPort = atoi(input.c_str());

    std::cout << "Enter audio port (default 9634): ";
    std::getline(std::cin, input);
    if (!input.empty()) audioPort = atoi(input.c_str());

    std::cin.clear();
}

int main(int argc, char* argv[]) {
    std::string mode;

    CoInitialize(NULL);

    if (argc < 2) {
        std::cout << "No command line arguments provided.\n";

        int choice = getUserChoice();

        if (choice == 1) {
            mode = "server";
            int vp = 9632, cp = 9633, wp = 8080, ap = 9634;
            getServerConfig(vp, cp, wp, ap);

            Server s(vp, cp, wp, ap);
            if (!s.start()) { 
                std::cerr<<"Failed to start server\n"; 
                CoUninitialize();
                return 1; 
            }
            std::cout<<"Server running. Press Enter to stop...\n";
            std::string dummy;
            std::getline(std::cin, dummy);
            s.stop();
            CoUninitialize();
            return 0;
        } else if (choice == 2) {
            mode = "client";
            std::string ip = "127.0.0.1";
            int vp = 9632, cp = 9633, ap = 9634;
            getClientConfig(ip, vp, cp, ap);

            Client c(ip, vp, cp, ap);
            if (!c.start()) { 
                std::cerr<<"Failed to start client\n"; 
                CoUninitialize();
                return 1; 
            }
            std::cout << "Client started. Waiting for video stream...\n";

            if (!c.waitForWindow()) {
                std::cerr << "Timeout waiting for video stream\n";
                c.stop();
                CoUninitialize();
                return 1;
            }

            std::cout << "Connected! Use the 'ISS Remote View' window to control the server.\n";
            std::cout << "- Move mouse to move server cursor\n";
            std::cout << "- Click to click on server\n";
            std::cout << "- Type to send keyboard input\n";

            MSG msg;
            while (GetMessage(&msg, NULL, 0, 0)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            c.stop();
            CoUninitialize();
            return 0;
        } else {
            std::cerr << "Invalid choice.\n";
            printUsage();
            CoUninitialize();
            return 1;
        }
    } else {
        mode = argv[1];

        if (mode == "server") {
            int vp = 9632, cp = 9633, wp = 8080, ap = 9634;
            if (argc >= 3) vp = atoi(argv[2]);
            if (argc >= 4) cp = atoi(argv[3]);
            if (argc >= 5) wp = atoi(argv[4]);
            if (argc >= 6) ap = atoi(argv[5]);

            Server s(vp, cp, wp, ap);
            if (!s.start()) { 
                std::cerr<<"Failed to start server\n"; 
                CoUninitialize();
                return 1; 
            }
            std::cout<<"Server running. Press Enter to stop...\n"; std::cin.get();
            s.stop();
            CoUninitialize();
            return 0;
        } else if (mode == "client") {
            if (argc < 3) { printUsage(); CoUninitialize(); return 1; }
            std::string ip = argv[2];
            int vp = 9632, cp = 9633, ap = 9634;
            if (argc >= 4) vp = atoi(argv[3]);
            if (argc >= 5) cp = atoi(argv[4]);
            if (argc >= 6) ap = atoi(argv[5]);

            Client c(ip, vp, cp, ap);
            if (!c.start()) { 
                std::cerr<<"Failed to start client\n"; 
                CoUninitialize();
                return 1; 
            }

            if (!c.waitForWindow()) {
                std::cerr << "Timeout waiting for video stream\n";
                c.stop();
                CoUninitialize();
                return 1;
            }

            std::cout << "Connected! Use the 'ISS Remote View' window to control the server.\n";
            std::cout << "- Move mouse to move server cursor\n";
            std::cout << "- Click to click on server\n";
            std::cout << "- Type to send keyboard input\n";

            MSG msg;
            while (GetMessage(&msg, NULL, 0, 0)) {
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            }

            c.stop();
            CoUninitialize();
            return 0;
        } else {
            printUsage();
            CoUninitialize();
            return 1;
        }
    }
    CoUninitialize();
    return 0;
}