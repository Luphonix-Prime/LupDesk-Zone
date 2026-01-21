// ISS_stream_control.cpp
// Build: cl /EHsc ISS_stream_control.cpp /link Gdiplus.lib Ws2_32.lib Gdi32.lib User32.lib Ole32.lib
// NOTE: Run vcvars64.bat before compiling for x64 build.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <objidl.h>
#include <gdiplus.h>
#include <comdef.h>

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
bool EncodeHBITMAPToJPEGBytes(HBITMAP hBmp, RECT srcRect, std::vector<BYTE>& out, ULONG quality=60) {
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

// ---------- server (stream + control) ----------
class Server {
public:
    Server(int portVideo=9632, int portControl=9633, int portWeb=8080) :
        m_portVideo(portVideo), m_portControl(portControl), m_portWeb(portWeb),
        m_listenVideo(INVALID_SOCKET), m_listenControl(INVALID_SOCKET), m_listenWeb(INVALID_SOCKET),
        m_clientVideo(INVALID_SOCKET), m_clientControl(INVALID_SOCKET),
        m_running(false) {}

    bool start() {
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

        // Get local IP address
        char hostname[256];
        gethostname(hostname, sizeof(hostname));
        hostent* host = gethostbyname(hostname);
        char* ip = inet_ntoa(*(in_addr*)host->h_addr_list[0]);

        std::cout << "Server IP: " << ip << "\n";
        std::cout << "Server listening video:" << m_portVideo << " control:" << m_portControl << " web:" << m_portWeb << "\n";
        
        // Start the web server thread immediately
        m_running = true;
        m_threadWeb = std::thread(&Server::webServerLoop, this);
        
        // Now wait for video and control clients
        std::cout << "Waiting for video and control clients...\n";
        sockaddr_in cli; int len = sizeof(cli);
        m_clientVideo = accept(m_listenVideo, (sockaddr*)&cli, &len);
        if (m_clientVideo == INVALID_SOCKET) { std::cerr<<"accept video failed\n"; return false; }
        std::cout<<"Video client connected\n";

        len = sizeof(cli);
        m_clientControl = accept(m_listenControl, (sockaddr*)&cli, &len);
        if (m_clientControl == INVALID_SOCKET) { std::cerr<<"accept control failed\n"; return false; }
        std::cout<<"Control client connected\n";

        // non-blocking basic timeout
        DWORD timeout = 5000;
        setsockopt(m_clientVideo, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout, sizeof(timeout));
        setsockopt(m_clientControl, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout, sizeof(timeout));

        m_threadCapture = std::thread(&Server::captureLoop, this);
        m_threadControl = std::thread(&Server::controlLoop, this);
        return true;
    }

    void stop() {
        m_running = false;
        if (m_threadCapture.joinable()) m_threadCapture.join();
        if (m_threadControl.joinable()) m_threadControl.join();
        if (m_threadWeb.joinable()) m_threadWeb.join();
        if (m_clientVideo != INVALID_SOCKET) closesocket(m_clientVideo);
        if (m_clientControl != INVALID_SOCKET) closesocket(m_clientControl);
        if (m_listenVideo != INVALID_SOCKET) closesocket(m_listenVideo);
        if (m_listenControl != INVALID_SOCKET) closesocket(m_listenControl);
        if (m_listenWeb != INVALID_SOCKET) closesocket(m_listenWeb);
        WSACleanup();
    }

private:
    int m_portVideo, m_portControl, m_portWeb;
    SOCKET m_listenVideo, m_listenControl, m_listenWeb, m_clientVideo, m_clientControl;
    std::atomic<bool> m_running;
    std::thread m_threadCapture, m_threadControl, m_threadWeb;
    std::mutex m_webMutex;
    std::vector<BYTE> m_latestFrame;
    bool m_frameUpdated = false;

    const int TILE_W = 256, TILE_H = 256;
    std::unordered_map<uint64_t, uint32_t> prevChecksums;

    void captureLoop() {
        // GDI+ init for jpeg encode
        GdiplusStartupInput gdiIn; ULONG_PTR token = 0; 
        if (GdiplusStartup(&token, &gdiIn, NULL) != Ok) {
            std::cerr << "GdiplusStartup failed\n";
            return;
        }

        HDC hScreen = GetDC(NULL);
        if (!hScreen) {
            std::cerr << "GetDC failed\n";
            GdiplusShutdown(token);
            return;
        }
        
        int screenW = GetSystemMetrics(SM_CXSCREEN);
        int screenH = GetSystemMetrics(SM_CYSCREEN);
        if (screenW <= 0 || screenH <= 0) {
            std::cerr << "Invalid screen dimensions\n";
            ReleaseDC(NULL, hScreen);
            GdiplusShutdown(token);
            return;
        }
        
        HDC hMem = CreateCompatibleDC(hScreen);
        if (!hMem) {
            std::cerr << "CreateCompatibleDC failed\n";
            ReleaseDC(NULL, hScreen);
            GdiplusShutdown(token);
            return;
        }
        
        HBITMAP hBmp = CreateCompatibleBitmap(hScreen, screenW, screenH);
        if (!hBmp) {
            std::cerr << "CreateCompatibleBitmap failed\n";
            DeleteDC(hMem);
            ReleaseDC(NULL, hScreen);
            GdiplusShutdown(token);
            return;
        }
        
        HGDIOBJ old = SelectObject(hMem, hBmp);
        if (old == HGDI_ERROR) {
            std::cerr << "SelectObject failed\n";
            DeleteObject(hBmp);
            DeleteDC(hMem);
            ReleaseDC(NULL, hScreen);
            GdiplusShutdown(token);
            return;
        }

        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = screenW; bi.bmiHeader.biHeight = -screenH;
        bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        std::vector<BYTE> fullBuf(screenW * screenH * 4);

        while (m_running) {
            if (!BitBlt(hMem, 0,0, screenW, screenH, hScreen, 0,0, SRCCOPY)) { 
                std::cerr<<"BitBlt failed\n"; 
                break; 
            }
            
            // overlay cursor
            CURSORINFO ci{}; ci.cbSize = sizeof(ci);
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
                ICONINFO ii; 
                if (GetIconInfo(ci.hCursor, &ii)) {
                    DrawIconEx(hMem, ci.ptScreenPos.x - ii.xHotspot, ci.ptScreenPos.y - ii.yHotspot, ci.hCursor, 0,0,0,NULL,DI_NORMAL);
                    if (ii.hbmMask) DeleteObject(ii.hbmMask);
                    if (ii.hbmColor) DeleteObject(ii.hbmColor);
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
                        
                        // make tile HBITMAP
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
                        if (!EncodeHBITMAPToJPEGBytes(tileBmp, rc, jpg, 60)) {
                            // fallback: try whole tile encode anyway (rare)
                            EncodeHBITMAPToJPEGBytes(tileBmp, rc, jpg, 50);
                        }
                        DeleteObject(tileBmp);
                        if (!jpg.empty()) changed.push_back({tx,ty,w,h, std::move(jpg)});
                    }
            }
        }

        // send if changed
        if (!changed.empty() && m_clientVideo != INVALID_SOCKET && m_running) {
            uint32_t magic = 0x49535332; // "ISS2" version 2
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
        
        // Save the full frame for web streaming (every 5 frames)
        static int frameCounter = 0;
        if (++frameCounter % 5 == 0) {
            // Create a full screenshot HBITMAP
            HBITMAP fullBmp = CreateCompatibleBitmap(hScreen, screenW, screenH);
            if (fullBmp) {
                HDC fullDc = CreateCompatibleDC(hScreen);
                if (fullDc) {
                    HGDIOBJ oldFull = SelectObject(fullDc, fullBmp);
                    if (oldFull != HGDI_ERROR) {
                        BitBlt(fullDc, 0, 0, screenW, screenH, hMem, 0, 0, SRCCOPY);
                        
                        // Encode to JPEG for web streaming
                        RECT fullRc{0, 0, screenW, screenH};
                        std::vector<BYTE> fullJpeg;
                        if (EncodeHBITMAPToJPEGBytes(fullBmp, fullRc, fullJpeg, 50)) {
                            // Save to latest frame for web server
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
        
        // Check if we should still be running
        if (!m_running) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(40)); // ~25FPS
    }

    // Cleanup
    SelectObject(hMem, old);
    DeleteObject(hBmp);
    DeleteDC(hMem);
    ReleaseDC(NULL, hScreen);
    if (token) GdiplusShutdown(token);
}

    void controlLoop() {
        // Listen for control messages from client on m_clientControl
        // Simple protocol:
        // uint8_t type (1=mouse move,2=mouse down,3=mouse up,4=key), then payload:
        // mouse move: int32 x,int32 y
        // mouse down/up: uint8_t button (1=left,2=right), int32 x,int32 y
        // key: uint8_t isDown (1=down,0=up), uint16 vk
        while (m_running) {
            uint8_t type;
            int r = recvAll(m_clientControl, (char*)&type, 1);
            if (r != 1) break;
            
            if (type == 1) {
                int32_t x,y; 
                if (recvAll(m_clientControl, (char*)&x,4) != 4) break;
                if (recvAll(m_clientControl, (char*)&y,4) != 4) break;
                // set mouse position
                SetCursorPos(x, y);
            } else if (type == 2 || type == 3) {
                uint8_t btn; int32_t x,y;
                if (recvAll(m_clientControl, (char*)&btn,1) != 1) break;
                if (recvAll(m_clientControl, (char*)&x,4) != 4) break;
                if (recvAll(m_clientControl, (char*)&y,4) != 4) break;
                
                // Validate button value
                if (btn != 1 && btn != 2) continue;
                
                // build INPUT sequence
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
                
                // Validate isDown value
                if (isDown != 0 && isDown != 1) continue;
                
                INPUT in; ZeroMemory(&in, sizeof(in));
                in.type = INPUT_KEYBOARD;
                in.ki.wVk = vk;
                in.ki.dwFlags = isDown ? 0 : KEYEVENTF_KEYUP;
                SendInput(1, &in, sizeof(INPUT));
            } else {
                // unknown type -> ignore and continue
                continue;
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
                // Small delay to prevent busy waiting
                Sleep(10);
                continue;
            }

            // Read HTTP request
            char buffer[4096];
            int bytes = recv(client, buffer, sizeof(buffer) - 1, 0);
            if (bytes <= 0) {
                closesocket(client);
                continue;
            }
            buffer[bytes] = '\0';

            // Null-terminate at the end of the first line to simplify parsing
            char* endOfLine = strchr(buffer, '\n');
            if (endOfLine) *(endOfLine + 1) = '\0';

            // Check if it's a request for the root page
            if (strncmp(buffer, "GET / ", 6) == 0 || strncmp(buffer, "GET / HTTP/1.1", 14) == 0) {
                // Serve HTML page
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
            // Check if it's a request for the MJPEG stream
            else if (strncmp(buffer, "GET /stream HTTP/1.1", 21) == 0) {
                // Send MJPEG header
                std::string header = "HTTP/1.1 200 OK\r\n";
                header += "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n";
                header += "Connection: close\r\n";
                header += "\r\n";
                send(client, header.c_str(), header.length(), 0);
                
                // Stream MJPEG data
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
                    } // Lock scope ends here
                    Sleep(100); // ~10 FPS
                }
            }
            // Handle other requests
            else {
                const char* notFound = "HTTP/1.1 404 Not Found\r\n\r\n";
                send(client, notFound, strlen(notFound), 0);
            }
            
            closesocket(client);
        }
    }
};

// ---------- client (receive + send control) ----------
class ClientWindow {
public:
    ClientWindow() : hwnd(NULL), dib(NULL), dibPixels(nullptr), width(0), height(0) {}
    ~ClientWindow(){ destroy(); }
    bool create(int w, int h) {
        if (w <= 0 || h <= 0) return false;
        width = w; height = h;
        
        WNDCLASS wc{}; 
        wc.lpfnWndProc = WndProcStatic; 
        wc.hInstance = GetModuleHandle(NULL); 
        wc.lpszClassName = "ISSClientWndClass";
        
        // Register class only if not already registered
        if (!GetClassInfo(wc.hInstance, wc.lpszClassName, &wc)) {
            if (!RegisterClass(&wc)) {
                std::cerr << "Failed to register window class\n";
                return false;
            }
        }
        
        hwnd = CreateWindow(wc.lpszClassName, "ISS Remote View", WS_OVERLAPPEDWINDOW,
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
        
        return dib != NULL;
    }
    void destroy() { if (dib) { DeleteObject(dib); dib=nullptr; dibPixels=nullptr; } if (hwnd) { DestroyWindow(hwnd); hwnd=NULL; } }
    HWND getHWND() const { return hwnd; }
    void updateTile(int x, int y, HBITMAP tile) {
        if (!dib || !tile) return;
        BITMAP tb; GetObject(tile, sizeof(tb), &tb);
        int tw = tb.bmWidth, th = tb.bmHeight;
        // get tile pixels
        BITMAPINFO bi{}; bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = tw; bi.bmiHeader.biHeight = -th; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
        std::vector<BYTE> buf(tw * th * 4);
        HDC dc = GetDC(NULL);
        HDC mdc = CreateCompatibleDC(dc);
        HGDIOBJ old = SelectObject(mdc, tile);
        GetDIBits(mdc, tile, 0, th, buf.data(), &bi, DIB_RGB_COLORS);
        SelectObject(mdc, old);
        DeleteDC(mdc); ReleaseDC(NULL, dc);
        // write into dibPixels
        for (int row=0; row<th; ++row) {
            memcpy((BYTE*)dibPixels + ((y+row) * width + x) * 4, buf.data() + row * tw * 4, tw * 4);
        }
        InvalidateRect(hwnd, NULL, FALSE);
    }
private:
    HWND hwnd; HBITMAP dib; void* dibPixels; int width, height;
    static LRESULT CALLBACK WndProcStatic(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
        if (msg == WM_CREATE) {
            CREATESTRUCT* cs = (CREATESTRUCT*)lp;
            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
            return 0;
        }
        ClientWindow* self = (ClientWindow*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (!self) return DefWindowProc(hWnd, msg, wp, lp);
        if (msg == WM_PAINT) {
            PAINTSTRUCT ps; HDC dc = BeginPaint(hWnd, &ps);
            if (self->dib) {
                HDC mem = CreateCompatibleDC(dc);
                HGDIOBJ old = SelectObject(mem, self->dib);
                BITMAP bm; GetObject(self->dib, sizeof(bm), &bm);
                StretchBlt(dc, 0,0, ps.rcPaint.right-ps.rcPaint.left, ps.rcPaint.bottom-ps.rcPaint.top, mem, 0,0, bm.bmWidth, bm.bmHeight, SRCCOPY);
                SelectObject(mem, old); DeleteDC(mem);
            }
            EndPaint(hWnd, &ps);
            return 0;
        } else if (msg == WM_DESTROY) {
            PostQuitMessage(0);
            return 0;
        }
        return DefWindowProc(hWnd, msg, wp, lp);
    }
};

// Control sender: reads native mouse/keyboard events from window and sends to server
class Client {
public:
    Client(const std::string& ip, int portVideo=9632, int portControl=9633) :
        m_ip(ip), m_portVideo(portVideo), m_portControl(portControl),
        m_sockVideo(INVALID_SOCKET), m_sockControl(INVALID_SOCKET), m_running(false) {}

    bool start() {
        WSADATA w; if (WSAStartup(MAKEWORD(2,2), &w) != 0) return false;
        // connect video
        m_sockVideo = socket(AF_INET, SOCK_STREAM, 0);
        if (m_sockVideo == INVALID_SOCKET) return false;
        sockaddr_in srv{}; srv.sin_family = AF_INET; srv.sin_port = htons(m_portVideo);
        InetPton(AF_INET, m_ip.c_str(), &srv.sin_addr);
        if (connect(m_sockVideo, (sockaddr*)&srv, sizeof(srv)) == SOCKET_ERROR) { closesocket(m_sockVideo); return false; }
        // connect control
        m_sockControl = socket(AF_INET, SOCK_STREAM, 0);
        if (m_sockControl == INVALID_SOCKET) { closesocket(m_sockVideo); return false; }
        srv.sin_port = htons(m_portControl);
        if (connect(m_sockControl, (sockaddr*)&srv, sizeof(srv)) == SOCKET_ERROR) { closesocket(m_sockVideo); closesocket(m_sockControl); return false; }

        DWORD tout = 5000;
        setsockopt(m_sockVideo, SOL_SOCKET, SO_RCVTIMEO, (char*)&tout, sizeof(tout));
        setsockopt(m_sockControl, SOL_SOCKET, SO_SNDTIMEO, (char*)&tout, sizeof(tout));

        m_running = true;
        m_threadRecv = std::thread(&Client::recvLoop, this);
        // control sending is event-driven: we'll install a simple mouse hook on the window thread (see below)
        return true;
    }

    void stop() {
        m_running = false;
        if (m_threadRecv.joinable()) m_threadRecv.join();
        if (m_sockVideo != INVALID_SOCKET) closesocket(m_sockVideo);
        if (m_sockControl != INVALID_SOCKET) closesocket(m_sockControl);
        WSACleanup();
    }

    // send mouse move
    void sendMouseMove(int x, int y) {
        if (m_sockControl == INVALID_SOCKET) return;
        uint8_t t = 1;
        char buf[1+8]; buf[0]=(char)t; memcpy(buf+1,&x,4); memcpy(buf+5,&y,4);
        sendAll(m_sockControl, buf, sizeof(buf));
    }
    // send mouse down/up
    void sendMouseButton(uint8_t downOrUp, uint8_t button, int x, int y) {
        if (m_sockControl == INVALID_SOCKET) return;
        uint8_t t = downOrUp; // 2=down,3=up
        char buf[1+1+8]; buf[0]=(char)t; buf[1]=(char)button; memcpy(buf+2,&x,4); memcpy(buf+6,&y,4);
        sendAll(m_sockControl, buf, sizeof(buf));
    }
    // send key
    void sendKey(uint8_t isDown, uint16_t vk) {
        if (m_sockControl == INVALID_SOCKET) return;
        char buf[1+1+2]; buf[0]=4; buf[1]=isDown; memcpy(buf+2,&vk,2);
        sendAll(m_sockControl, buf, sizeof(buf));
    }

    // Create & show UI window (this runs in caller thread)
    bool createWindowAndRun() {
        // in recvLoop, we will create Window once first frame known, but for control events we want local window to capture input
        // We'll create a simple control window to capture mouse/keyboard and forward events to server.
        WNDCLASS wc{}; wc.lpfnWndProc = StaticWndProc; wc.hInstance = GetModuleHandle(NULL); wc.lpszClassName = "ISSClientControl";
        RegisterClass(&wc);
        HWND hwnd = CreateWindow(wc.lpszClassName, "ISS Client Control (click window to focus)", WS_OVERLAPPEDWINDOW,
                                 CW_USEDEFAULT, CW_USEDEFAULT, 900, 600, NULL, NULL, wc.hInstance, this);
        if (!hwnd) return false;
        ShowWindow(hwnd, SW_SHOW); UpdateWindow(hwnd);

        // message loop for UI events (mouse/keyboard) -> forward into control socket
        MSG msg;
        // Keep the window alive until WM_QUIT is received
        while (true) {
            if (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
                if (msg.message == WM_QUIT) break;
                TranslateMessage(&msg);
                DispatchMessage(&msg);
            } else {
                // Small delay to prevent busy-waiting
                Sleep(1);
            }
        }
        return true;
    }

private:
    std::string m_ip; int m_portVideo, m_portControl;
    SOCKET m_sockVideo, m_sockControl;
    std::atomic<bool> m_running;
    std::thread m_threadRecv;
    ClientWindow renderWnd;

    static LRESULT CALLBACK StaticWndProc(HWND hWnd, UINT msg, WPARAM wp, LPARAM lp) {
        Client* self = (Client*)GetWindowLongPtr(hWnd, GWLP_USERDATA);
        if (msg == WM_CREATE) {
            CREATESTRUCT* cs = (CREATESTRUCT*)lp;
            self = (Client*)cs->lpCreateParams;
            SetWindowLongPtr(hWnd, GWLP_USERDATA, (LONG_PTR)self);
            return 0;
        }
        if (!self) return DefWindowProc(hWnd, msg, wp, lp);
        switch (msg) {
            case WM_MOUSEMOVE: {
                int x = LOWORD(lp), y = HIWORD(lp);
                self->sendMouseMove(x,y);
                return 0;
            }
            case WM_LBUTTONDOWN: {
                int x=LOWORD(lp), y=HIWORD(lp);
                self->sendMouseButton(2, 1, x, y); // down, left
                return 0;
            }
            case WM_LBUTTONUP: {
                int x=LOWORD(lp), y=HIWORD(lp);
                self->sendMouseButton(3, 1, x, y); // up, left
                return 0;
            }
            case WM_RBUTTONDOWN: {
                int x=LOWORD(lp), y=HIWORD(lp);
                self->sendMouseButton(2, 2, x, y); // down, right
                return 0;
            }
            case WM_RBUTTONUP: {
                int x=LOWORD(lp), y=HIWORD(lp);
                self->sendMouseButton(3, 2, x, y); // up, right
                return 0;
            }
            case WM_KEYDOWN: {
                uint16_t vk = (uint16_t)wp;
                self->sendKey(1, vk);
                return 0;
            }
            case WM_KEYUP: {
                uint16_t vk = (uint16_t)wp;
                self->sendKey(0, vk);
                return 0;
            }
        }
        return DefWindowProc(hWnd, msg, wp, lp);
    }

    void recvLoop() {
        // Receive frames and render into window
        GdiplusStartupInput gi; ULONG_PTR token = 0;
        if (GdiplusStartup(&token, &gi, NULL) != Ok) {
            std::cerr << "Client GdiplusStartup failed\n";
            return;
        }
        
        while (m_running) {
            uint32_t magic = 0;
            int r = recvAll(m_sockVideo, (char*)&magic, 4);
            if (r != 4) break;
            if (magic != 0x49535332) { std::cerr<<"Bad magic\n"; break; }
            
            uint32_t w,h,tW,tH,count;
            if (recvAll(m_sockVideo, (char*)&w,4) != 4) break;
            if (recvAll(m_sockVideo, (char*)&h,4) != 4) break;
            if (recvAll(m_sockVideo, (char*)&tW,4) != 4) break;
            if (recvAll(m_sockVideo, (char*)&tH,4) != 4) break;
            if (recvAll(m_sockVideo, (char*)&count,4) != 4) break;
            
            // Validate values
            if (w > 10000 || h > 10000 || tW > 1000 || tH > 1000 || count > 10000) {
                std::cerr << "Invalid frame data\n";
                break;
            }
            
            if (!renderWnd.getHWND()) {
                // create render window with size w,h inside our control window area (or separate)
                if (!renderWnd.create((int)w, (int)h)) {
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
                
                // Validate values
                if (tx > w || ty > h || tw > tW || th > tH || sz == 0 || sz > 100*1024*1024) {
                    std::cerr << "Invalid tile data\n";
                    continue;  // Skip this tile but continue with others
                }
                
                std::vector<BYTE> data(sz);
                if (recvAll(m_sockVideo, (char*)data.data(), (int)sz) != (int)sz) break;
                
                HBITMAP tile = DecodeJPEGBytesToHBITMAP(data.data(), data.size());
                if (tile) {
                    renderWnd.updateTile((int)tx, (int)ty, tile);
                    DeleteObject(tile);
                }
            }
            
            // process pending messages (so that renderWnd paints)
            MSG msg;
            while (PeekMessage(&msg, NULL, 0,0, PM_REMOVE)) { 
                TranslateMessage(&msg); 
                DispatchMessage(&msg); 
            }
        }
        
        if (token) GdiplusShutdown(token);
    }
};

// ---------- main handling ----------
void printUsage() {
    std::cout<<"Usage:\n  server [videoPort] [controlPort] [webPort]\n  client <serverIP> [videoPort] [controlPort]\n";
}

// Function to get user choice when double-clicked
int getUserChoice() {
    std::cout << "Select mode:\n";
    std::cout << "1. Server\n";
    std::cout << "2. Client\n";
    std::cout << "Enter choice (1 or 2): ";
    
    int choice;
    std::cin >> choice;
    // Clear the input buffer
    std::cin.clear();
    std::cin.ignore(10000, '\n');
    return choice;
}

// Function to get server configuration
void getServerConfig(int& videoPort, int& controlPort, int& webPort) {
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
    
    // Ensure we clear any remaining input
    std::cin.clear();
}

// Function to get client configuration
void getClientConfig(std::string& serverIP, int& videoPort, int& controlPort) {
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
    
    // Ensure we clear any remaining input
    std::cin.clear();
}

int main(int argc, char* argv[]) {
    std::string mode;
    
    // Initialize COM for GDI+
    CoInitialize(NULL);
    
    if (argc < 2) {
        // No command line arguments - assume double-clicked
        std::cout << "No command line arguments provided.\n";
        
        int choice = getUserChoice();
        
        if (choice == 1) {
            mode = "server";
            int vp = 9632, cp = 9633, wp = 8080;
            getServerConfig(vp, cp, wp);
            
            Server s(vp, cp, wp);
            if (!s.start()) { 
                std::cerr<<"Failed to start server\n"; 
                CoUninitialize();
                return 1; 
            }
            std::cout<<"Server running. Close window to stop...\n";
            // Wait for user to press Enter or close window
            std::string dummy;
            std::getline(std::cin, dummy);
            s.stop();
            CoUninitialize();
            return 0;
        } else if (choice == 2) {
            mode = "client";
            std::string ip = "127.0.0.1";
            int vp = 9632, cp = 9633;
            getClientConfig(ip, vp, cp);
            
            Client c(ip, vp, cp);
            if (!c.start()) { 
                std::cerr<<"Failed to start client\n"; 
                CoUninitialize();
                return 1; 
            }
            std::cout << "Client started. Click on the client window to interact with the remote screen.\n";
            std::cout << "Close the client window to stop...\n";
            // create local control window that will also display remote view
            c.createWindowAndRun();
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
        // Command line arguments provided
        mode = argv[1];
        
        if (mode == "server") {
            int vp = 9632, cp = 9633, wp = 8080;
            if (argc >= 3) vp = atoi(argv[2]);
            if (argc >= 4) cp = atoi(argv[3]);
            if (argc >= 5) wp = atoi(argv[4]);
            
            Server s(vp, cp, wp);
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
            int vp = 9632, cp = 9633;
            if (argc >= 4) vp = atoi(argv[3]);
            if (argc >= 5) cp = atoi(argv[4]);
            
            Client c(ip, vp, cp);
            if (!c.start()) { 
                std::cerr<<"Failed to start client\n"; 
                CoUninitialize();
                return 1; 
            }
            // create local control window that will also display remote view (simple combined approach)
            // The client has two windows: a control window (to capture input) and the render window created by recvLoop.
            c.createWindowAndRun();
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
