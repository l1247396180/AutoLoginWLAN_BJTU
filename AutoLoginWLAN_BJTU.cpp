// ============================================================================
//  校园网自动登录程序（单文件版）
//  功能：开机自启动 / 事件驱动低占用 / 网络识别 / 互联网检测 / IP校验 / 自动登录 / 日志
//  编译：g++ main.cpp -o AutoLogin.exe -lole32 -loleaut32 -luuid -lwinhttp -ladvapi32 -liphlpapi -lws2_32 -mwindows
// ============================================================================

#define _CRT_SECURE_NO_WARNINGS
#include <winsock2.h>
#include <windows.h>
#include <netlistmgr.h>
#include <winhttp.h>
#include <iphlpapi.h>
#include <ws2tcpip.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <mutex>
#include <fstream>
#include <sstream>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "uuid.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

// ================================ 配置区 ================================
const std::wstring	CAMPUS_NETWORK_NAME=L"web.wlan.bjtu";     	// 【必改】网络和共享中心看到的网络名
const std::wstring	CAMPUS_IP_PREFIX=L"";             			// 【建议改】校园网网段前缀，留空则不校验
const std::string	CAMPUS_USER_ACCOUNT="";             		// 账号
const std::string	CAMPUS_USER_PASSWORD="";   				  	// 密码（建议后续加密存储）
const std::wstring	CAMPUS_LOGIN_HOST=L"10.10.42.3";          	// 认证服务器
const INTERNET_PORT CAMPUS_LOGIN_PORT=801;
const std::wstring	CONNECTIVITY_TEST_URL=L"https://www.msftconnecttest.com/connecttest.txt";
const ULONGLONG		LOGIN_THROTTLE_MS=60000;                  	// 登录节流
// =======================================================================

// ================================ 日志 ==================================
std::mutex g_logMutex;

void WriteLog(const std::string& msg)
{
	std::lock_guard<std::mutex> lock(g_logMutex);
	SYSTEMTIME st; GetLocalTime(&st);
	static std::wstring logPath;
	if(logPath.empty())
	{
		wchar_t exePath[MAX_PATH]={};
		GetModuleFileNameW(nullptr,exePath,MAX_PATH);
		std::wstring s(exePath);
		size_t p=s.find_last_of(L"\\/");
		s=(p==std::wstring::npos)?L".":s.substr(0,p);
		logPath=s+L"\\AutoLogin.log";
	}
	std::ofstream f(logPath.c_str(),std::ios::app);
	if(!f) return;
	char ts[32];
	std::snprintf(ts,sizeof(ts),"%04d-%02d-%02d %02d:%02d:%02d",
		st.wYear,st.wMonth,st.wDay,st.wHour,st.wMinute,st.wSecond);
	f<<"["<<ts<<"] "<<msg<<"\r\n";
}

// ============================ URL 编码 ==================================
std::string UrlEncode(const std::string& s)
{
	std::string r;
	for(unsigned char c:s)
	{
		if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~') r+=(char)c;
		else { char b[4]; std::snprintf(b,sizeof(b),"%%%02X",c); r+=b; }
	}
	return r;
}

// ============================ GUID 字符串 ==============================
std::wstring GuidToWString(const GUID& g)
{
	wchar_t buf[64];
	std::swprintf(buf,64,L"{%08X-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
		g.Data1,g.Data2,g.Data3,
		g.Data4[0],g.Data4[1],g.Data4[2],g.Data4[3],
		g.Data4[4],g.Data4[5],g.Data4[6],g.Data4[7]);
	return std::wstring(buf);
}

// ===================== IP 校验（核心新增功能） =========================
// 判断字符串是否为合法 IPv4
bool IsValidIPv4(const std::wstring& ip)
{
	IN_ADDR addr;
	std::string s(ip.begin(),ip.end());
	return InetPtonA(AF_INET,s.c_str(),&addr)==1;
}

// 判断是否为本机可用的"有效"IP（排除 APIPA/回环/0.0.0.0）
bool IsUsableIp(const std::wstring& ip)
{
	if(!IsValidIPv4(ip)) return false;
	if(ip==L"0.0.0.0")   return false;
	if(ip==L"127.0.0.1") return false;
	if(ip.rfind(L"169.254.",0)==0) return false;  // APIPA
	return true;
}

// 判断是否在校园网网段（前缀匹配）
bool IsInCampusRange(const std::wstring& ip)
{
	if(CAMPUS_IP_PREFIX.empty()) return true;
	return ip.rfind(CAMPUS_IP_PREFIX,0)==0;
}

// 枚举本机所有 IPv4（供校验时打印和交叉验证）
std::vector<std::wstring> GetAllLocalIPv4()
{
	std::vector<std::wstring> result;
	ULONG size=0;
	GetAdaptersAddresses(AF_INET,0,nullptr,nullptr,&size);
	if(size==0) return result;
	std::vector<BYTE> buf(size);
	auto pAddrs=reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
	if(GetAdaptersAddresses(AF_INET,0,nullptr,pAddrs,&size)!=NO_ERROR) return result;

	for(auto p=pAddrs; p; p=p->Next)
	{
		for(auto u=p->FirstUnicastAddress; u; u=u->Next)
		{
			if(u->Address.lpSockaddr->sa_family!=AF_INET) continue;
			auto sa=reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
			wchar_t ipStr[INET_ADDRSTRLEN]={};
			InetNtopW(AF_INET,&sa->sin_addr,ipStr,INET_ADDRSTRLEN);
			result.push_back(ipStr);
		}
	}
	return result;
}

// 从指定网卡读取 IPv4
std::wstring GetIpForAdapter(const GUID& adapterId)
{
	std::wstring guidW=GuidToWString(adapterId);
	std::string guidA(guidW.begin(),guidW.end());
	ULONG size=0;
	GetAdaptersAddresses(AF_INET,0,nullptr,nullptr,&size);
	if(size==0) return L"";
	std::vector<BYTE> buf(size);
	auto pAddrs=reinterpret_cast<PIP_ADAPTER_ADDRESSES>(buf.data());
	if(GetAdaptersAddresses(AF_INET,0,nullptr,pAddrs,&size)!=NO_ERROR) return L"";

	for(auto p=pAddrs; p; p=p->Next)
	{
		if(_stricmp(p->AdapterName,guidA.c_str())!=0) continue;
		for(auto u=p->FirstUnicastAddress; u; u=u->Next)
		{
			if(u->Address.lpSockaddr->sa_family!=AF_INET) continue;
			auto sa=reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr);
			wchar_t ipStr[INET_ADDRSTRLEN]={};
			InetNtopW(AF_INET,&sa->sin_addr,ipStr,INET_ADDRSTRLEN);
			std::wstring ip(ipStr);
			if(IsUsableIp(ip)) return ip;
		}
	}
	return L"";
}

// ★ 核心：检验本机 IP（多道关卡）
// 返回空字符串表示校验失败，非空表示最终可用的 IP
std::wstring VerifyLocalIp(const GUID& campusAdapterId)
{
	// 步骤1：日志列出本机所有 IPv4，便于排查
	auto allIps=GetAllLocalIPv4();
	std::string ipListLog="本机所有 IPv4: ";
	for(auto& ip:allIps)
	{
		ipListLog+=std::string(ip.begin(),ip.end())+" ";
	}
	WriteLog(ipListLog);

	// 步骤2：从校园网对应网卡读取 IP
	std::wstring ip=GetIpForAdapter(campusAdapterId);
	if(ip.empty())
	{
		WriteLog("[IP校验] 校园网网卡上未找到可用 IP（可能 DHCP 尚未完成）。");
		return L"";
	}
	WriteLog("[IP校验] 校园网网卡 IP: "+std::string(ip.begin(),ip.end()));

	// 步骤3：格式与可用性检查
	if(!IsValidIPv4(ip))
	{
		WriteLog("[IP校验] 失败：IP 格式非法。");
		return L"";
	}
	if(!IsUsableIp(ip))
	{
		WriteLog("[IP校验] 失败：IP 为 APIPA/回环/0.0.0.0 等无效值。");
		return L"";
	}

	// 步骤4：网段检查（防止取到其他虚拟网卡/代理网卡的 IP）
	if(!IsInCampusRange(ip))
	{
		WriteLog("[IP校验] 失败：IP 不在校园网网段 '"+
			std::string(CAMPUS_IP_PREFIX.begin(),CAMPUS_IP_PREFIX.end())+"' 内。");
		return L"";
	}

	// 步骤5：与全量枚举结果交叉验证，确认该 IP 确实属于本机
	bool found=false;
	for(auto& x:allIps) if(x==ip) { found=true; break; }
	if(!found)
	{
		WriteLog("[IP校验] 失败：取得的 IP 未出现在本机地址表中。");
		return L"";
	}

	WriteLog("[IP校验] 通过。");
	return ip;
}

// ============ 判据1：NLM 连通性标志（与 Windows 托盘图标一致） ============
bool CheckInternetViaNLM()
{
	INetworkListManager* pNLM=nullptr;
	if(FAILED(CoCreateInstance(CLSID_NetworkListManager,nullptr,CLSCTX_ALL,
		IID_INetworkListManager,(void**)&pNLM)))
	{
		WriteLog("[联网/NLM] 创建 NLM 失败");
		return false;
	}

	IEnumNetworks* pEnum=nullptr;
	if(FAILED(pNLM->GetNetworks(NLM_ENUM_NETWORK_CONNECTED,&pEnum)))
	{
		WriteLog("[联网/NLM] 枚举网络失败");
		pNLM->Release();
		return false;
	}

	bool hasInternet=false;
	INetwork* pNet=nullptr;
	ULONG fetched=0;
	while(pEnum->Next(1,&pNet,&fetched)==S_OK&&fetched==1)
	{
		BSTR name=nullptr;
		pNet->GetName(&name);
		std::wstring netName=name?name:L"(unknown)";
		if(name) SysFreeString(name);

		NLM_CONNECTIVITY conn=NLM_CONNECTIVITY_DISCONNECTED;
		if(SUCCEEDED(pNet->GetConnectivity(&conn)))
		{
			bool hasV4=(conn&NLM_CONNECTIVITY_IPV4_INTERNET)!=0;
			bool hasV6=(conn&NLM_CONNECTIVITY_IPV6_INTERNET)!=0;

			char hexbuf[16];
			std::snprintf(hexbuf,sizeof(hexbuf),"0x%08X",(unsigned)conn);
			WriteLog(std::string("[联网/NLM] 网络 '")+
				std::string(netName.begin(),netName.end())+
				"' conn="+hexbuf+
				" v4="+(hasV4?"Y":"N")+
				" v6="+(hasV6?"Y":"N"));

			if(hasV4||hasV6) hasInternet=true;
		}
		pNet->Release();
	}
	pEnum->Release();
	pNLM->Release();
	return hasInternet;
}

// ============ 判据2：HTTP 内容检测（禁止重定向 + 校验内容） ============
// expectContent 为 nullptr 时只接受 204（generate_204 标准成功）
bool TryHttpGet(const wchar_t* url,const char* expectContent)
{
	HINTERNET hSession=WinHttpOpen(L"AutoLogin/1.0",
		WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS,0);
	if(!hSession)
	{
		WriteLog("[联网/HTTP] WinHttpOpen 失败");
		return false;
	}

	DWORD to=6000;
	WinHttpSetOption(hSession,WINHTTP_OPTION_CONNECT_TIMEOUT,&to,sizeof(to));
	WinHttpSetOption(hSession,WINHTTP_OPTION_RECEIVE_TIMEOUT,&to,sizeof(to));
	WinHttpSetOption(hSession,WINHTTP_OPTION_SEND_TIMEOUT,&to,sizeof(to));

	URL_COMPONENTS uc={};
	uc.dwStructSize=sizeof(uc);
	uc.dwHostNameLength=(DWORD)-1;
	uc.dwUrlPathLength=(DWORD)-1;
	uc.dwSchemeLength=(DWORD)-1;
	if(!WinHttpCrackUrl(url,0,0,&uc))
	{
		WinHttpCloseHandle(hSession); return false;
	}
	std::wstring host(uc.lpszHostName,uc.dwHostNameLength);
	std::wstring path(uc.lpszUrlPath,uc.dwUrlPathLength);

	HINTERNET hConn=WinHttpConnect(hSession,host.c_str(),uc.nPort,0);
	if(!hConn) { WinHttpCloseHandle(hSession); return false; }

	DWORD flags=(uc.nScheme==INTERNET_SCHEME_HTTPS)?WINHTTP_FLAG_SECURE:0;
	HINTERNET hReq=WinHttpOpenRequest(hConn,L"GET",path.c_str(),
		NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,flags);
	if(!hReq) { WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession); return false; }

	// ★ 关键：禁止自动跟随重定向，这样被 captive portal 劫持时能看清 302
	DWORD policy=WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
	WinHttpSetOption(hReq,WINHTTP_OPTION_REDIRECT_POLICY,&policy,sizeof(policy));

	BOOL ok=WinHttpSendRequest(hReq,WINHTTP_NO_ADDITIONAL_HEADERS,0,
		WINHTTP_NO_REQUEST_DATA,0,0,0);
	if(ok) ok=WinHttpReceiveResponse(hReq,NULL);

	bool result=false;
	std::wstring urlW(url);
	std::string urlA(urlW.begin(),urlW.end());

	if(ok)
	{
		DWORD code=0,sz=sizeof(code);
		WinHttpQueryHeaders(hReq,WINHTTP_QUERY_STATUS_CODE|WINHTTP_QUERY_FLAG_NUMBER,
			WINHTTP_HEADER_NAME_BY_INDEX,&code,&sz,WINHTTP_NO_HEADER_INDEX);

		// 读响应内容（最多 4KB）
		std::string body;
		DWORD avail=0;
		do
		{
			avail=0;
			if(!WinHttpQueryDataAvailable(hReq,&avail)||avail==0) break;
			std::vector<char> buf(avail+1);
			DWORD rd=0;
			WinHttpReadData(hReq,buf.data(),avail,&rd);
			buf[rd]='\0';
			body+=buf.data();
			if(body.size()>4096) break;
		} while(avail>0);

		bool isHtml=body.find("<html")!=std::string::npos||
			body.find("<HTML")!=std::string::npos||
			body.find("<!DOCTYPE")!=std::string::npos;

		bool contentOk=(expectContent==nullptr)||
			(body.find(expectContent)!=std::string::npos);

		WriteLog("[联网/HTTP] "+urlA+
			" status="+std::to_string(code)+
			" bodyLen="+std::to_string(body.size())+
			(isHtml?" (疑似被劫持到登录页)":""));

		if(code==204&&body.empty())            result=true; // generate_204
		else if(code==200&&contentOk&&!isHtml) result=true;
		else if(code==302||code==301)
		{
			WriteLog("[联网/HTTP] 被重定向，判定为 captive portal 劫持");
		}
	}
	else
	{
		WriteLog("[联网/HTTP] "+urlA+
			" 请求失败，WinHTTP 错误码: "+std::to_string(GetLastError()));
	}

	WinHttpCloseHandle(hReq);
	WinHttpCloseHandle(hConn);
	WinHttpCloseHandle(hSession);
	return result;
}

bool CheckInternetViaHttp()
{
	if(TryHttpGet(L"http://connectivitycheck.gstatic.com/generate_204",nullptr))         return true;
	if(TryHttpGet(L"http://www.msftconnecttest.com/connecttest.txt","Microsoft Connect Test")) return true;
	if(TryHttpGet(L"http://captive.apple.com/hotspot-detect.html","Success"))            return true;
	return false;
}

// ============ 组合检测（对外接口，替换原 CheckInternetConnectivity） ============
bool CheckInternetConnectivity()
{
	bool nlm=CheckInternetViaNLM();
	bool http=CheckInternetViaHttp();
	WriteLog(std::string("[联网/汇总] NLM=")+(nlm?"Y":"N")+
		" HTTP="+(http?"Y":"N"));
	return nlm||http;
}

// ============================ 校园网登录 ==============================
void PerformCampusLogin(const std::wstring& localIpW)
{
	static ULONGLONG lastTick=0;
	static bool everTried=false;
	ULONGLONG now=GetTickCount64();
	if(everTried&&(now-lastTick)<LOGIN_THROTTLE_MS)
	{
		ULONGLONG remain=(LOGIN_THROTTLE_MS-(now-lastTick))/1000;
		WriteLog("距上次登录不足 "+std::to_string(LOGIN_THROTTLE_MS/1000)+
			" 秒（还剩约 "+std::to_string(remain)+" 秒），跳过。");
		return;
	}
	lastTick=now;
	everTried=true;

	std::string localIp(localIpW.begin(),localIpW.end());
	std::stringstream qs;
	qs<<"/eportal/portal/login"
		<<"?callback=dr1003"
		<<"&login_method=1"
		<<"&user_account="<<UrlEncode(CAMPUS_USER_ACCOUNT)
		<<"&user_password="<<UrlEncode(CAMPUS_USER_PASSWORD)
		<<"&wlan_user_ip="<<UrlEncode(localIp)
		<<"&wlan_user_ipv6="
		<<"&wlan_user_mac=000000000000"
		<<"&wlan_ac_ip="
		<<"&wlan_ac_name="
		<<"&jsVersion=4.2.1"
		<<"&terminal_type=1"
		<<"&lang=zh-cn"
		<<"&v="<<(rand()%9000+1000)
		<<"&lang=zh";
	std::string urlStr=qs.str();
	std::wstring urlPath(urlStr.begin(),urlStr.end());

	WriteLog("开始登录，使用 IP: "+localIp);
	WriteLog("请求 URL: "+urlStr);

	HINTERNET hSession=WinHttpOpen(L"AutoLogin/1.0",
		WINHTTP_ACCESS_TYPE_NO_PROXY,WINHTTP_NO_PROXY_NAME,
		WINHTTP_NO_PROXY_BYPASS,0);
	if(!hSession) { WriteLog("WinHttpOpen 失败"); return; }

	HINTERNET hConn=WinHttpConnect(hSession,CAMPUS_LOGIN_HOST.c_str(),CAMPUS_LOGIN_PORT,0);
	if(!hConn) { WinHttpCloseHandle(hSession); return; }

	HINTERNET hReq=WinHttpOpenRequest(hConn,L"GET",urlPath.c_str(),
		NULL,WINHTTP_NO_REFERER,WINHTTP_DEFAULT_ACCEPT_TYPES,0);
	if(!hReq) { WinHttpCloseHandle(hConn); WinHttpCloseHandle(hSession); return; }

	DWORD to=5000;
	WinHttpSetOption(hReq,WINHTTP_OPTION_CONNECT_TIMEOUT,&to,sizeof(to));
	WinHttpSetOption(hReq,WINHTTP_OPTION_RECEIVE_TIMEOUT,&to,sizeof(to));

	BOOL ok=WinHttpSendRequest(hReq,WINHTTP_NO_ADDITIONAL_HEADERS,0,
		WINHTTP_NO_REQUEST_DATA,0,0,0);
	if(ok) ok=WinHttpReceiveResponse(hReq,NULL);

	if(ok)
	{
		std::string resp;
		DWORD dw=0;
		do
		{
			dw=0;
			if(!WinHttpQueryDataAvailable(hReq,&dw)||dw==0) break;
			std::vector<char> b(dw+1);
			DWORD rd=0;
			WinHttpReadData(hReq,b.data(),dw,&rd);
			b[rd]='\0';
			resp+=b.data();
		} while(dw>0);

		WriteLog("登录响应: "+resp);

		if(resp.find("\"result\":\"1\"")!=std::string::npos||
			resp.find("\"result\":1")!=std::string::npos)
		{
			WriteLog("校园网登录成功。");
			bool online=false;
			for(int i=0; i<15; ++i)
			{   // 最多等 30 秒
				Sleep(2000);
				if(CheckInternetConnectivity()) { online=true; break; }
				WriteLog("复检第 "+std::to_string(i+1)+" 次仍未通过，继续等待...");
			}
			if(online) WriteLog("复检：互联网已可访问。");
			else        WriteLog("复检：超时（30 秒），请检查服务器策略或 DNS。");
		}
		else
		{
			WriteLog("登录失败，请检查账号密码或服务器状态。");
		}
	}
	else
	{
		WriteLog("登录请求失败，WinHTTP 错误码: "+std::to_string(GetLastError()));
	}

	WinHttpCloseHandle(hReq);
	WinHttpCloseHandle(hConn);
	WinHttpCloseHandle(hSession);
}

// ---------------- 重试定时器 ----------------
static const UINT_PTR RETRY_TIMER_ID=1;
static const UINT     RETRY_TIMER_INTERVAL_MS=30000;   // 30 秒
static bool           g_retryTimerRunning=false;

void EnsureRetryTimer(bool enable)
{
	if(enable&&!g_retryTimerRunning)
	{
		SetTimer(NULL,RETRY_TIMER_ID,RETRY_TIMER_INTERVAL_MS,NULL);
		g_retryTimerRunning=true;
		WriteLog("启动重试定时器（30 秒）。");
	}
	else if(!enable&&g_retryTimerRunning)
	{
		KillTimer(NULL,RETRY_TIMER_ID);
		g_retryTimerRunning=false;
		WriteLog("停止重试定时器。");
	}
}

// ============================ 核心检查 ==============================
struct ScopedBool
{
	bool& ref;
	ScopedBool(bool& r): ref(r) { ref=true; }
	~ScopedBool() { ref=false; }
};

void CheckAndAct()
{
	static bool      inProgress=false;
	static ULONGLONG lastCheckTick=0;

	if(inProgress) { WriteLog("CheckAndAct 正在执行，跳过重入。"); return; }

	// 3 秒内已检查过则跳过（防止 WiFi 抖动导致事件风暴）
	ULONGLONG now=GetTickCount64();
	if(lastCheckTick!=0&&(now-lastCheckTick)<3000) return;
	lastCheckTick=now;

	ScopedBool guard(inProgress);   // 所有 return 路径自动复位

	INetworkListManager* pNLM=nullptr;
	if(FAILED(CoCreateInstance(CLSID_NetworkListManager,nullptr,CLSCTX_ALL,
		IID_INetworkListManager,(void**)&pNLM)))
	{
		WriteLog("创建 NLM 失败"); return;
	}

	IEnumNetworks* pEnum=nullptr;
	if(FAILED(pNLM->GetNetworks(NLM_ENUM_NETWORK_CONNECTED,&pEnum)))
	{
		pNLM->Release(); return;
	}

	bool foundCampus=false;
	GUID campusAdapterId={};

	INetwork* pNet=nullptr;
	ULONG fetched=0;
	while(pEnum->Next(1,&pNet,&fetched)==S_OK&&fetched==1)
	{
		BSTR bstrName=nullptr;
		bool isCampus=false;
		if(SUCCEEDED(pNet->GetName(&bstrName))&&bstrName)
		{
			std::wstring netName(bstrName);
			SysFreeString(bstrName);
			if(netName==CAMPUS_NETWORK_NAME)
			{
				isCampus=true;
				foundCampus=true;
				WriteLog("已连接至校园网: "+std::string(netName.begin(),netName.end()));
			}
		}
		if(isCampus)
		{
			IEnumNetworkConnections* pConns=nullptr;
			if(SUCCEEDED(pNet->GetNetworkConnections(&pConns)))
			{
				INetworkConnection* pConn=nullptr;
				ULONG c=0;
				if(pConns->Next(1,&pConn,&c)==S_OK&&c==1)
				{
					pConn->GetAdapterId(&campusAdapterId);
					pConn->Release();
				}
				pConns->Release();
			}
		}
		pNet->Release();
	}
	pEnum->Release();
	pNLM->Release();

	if(!foundCampus)
	{
		WriteLog("未检测到校园网，程序退出。");
		EnsureRetryTimer(false);
		PostQuitMessage(0);
		return;
	}

	bool online=CheckInternetConnectivity();
	if(!online) { Sleep(1000); online=CheckInternetConnectivity(); }

	if(online)
	{
		WriteLog("互联网访问正常，无需登录。");
		EnsureRetryTimer(false);   // ★ 已联网，停掉重试
		return;
	}

	WriteLog("无法访问互联网，准备登录。");
	EnsureRetryTimer(true);        // ★ 未联网，确保有重试机会

	std::wstring ip=VerifyLocalIp(campusAdapterId);
	if(ip.empty())
	{
		WriteLog("IP 校验未通过，跳过本次登录。");
		return;
	}
	PerformCampusLogin(ip);
}

// ============================ NLM 事件 ==============================
class NetworkEventSink: public INetworkConnectionEvents
{
public:
	STDMETHODIMP QueryInterface(REFIID riid,void** ppv) override
	{
		if(riid==IID_IUnknown||riid==IID_INetworkConnectionEvents)
		{
			*ppv=static_cast<INetworkConnectionEvents*>(this);
			AddRef(); return S_OK;
		}
		*ppv=nullptr; return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&m_ref); }
	STDMETHODIMP_(ULONG) Release() override
	{
		LONG r=InterlockedDecrement(&m_ref);
		if(r==0) delete this;
		return r;
	}
	STDMETHODIMP NetworkConnectionConnectivityChanged(GUID,NLM_CONNECTIVITY) override
	{
		WriteLog("网络连通性变化事件。");
		CheckAndAct();
		return S_OK;
	}
	STDMETHODIMP NetworkConnectionPropertyChanged(GUID,NLM_CONNECTION_PROPERTY_CHANGE) override
	{
		return S_OK;
	}
private:
	LONG m_ref=1;
};

// ============================ 开机自启动 ==============================
bool SetAutoStart(bool enable)
{
	HKEY hKey;
	const wchar_t* regPath=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
	const wchar_t* valueName=L"MyCampusAutoLogin";
	if(RegOpenKeyExW(HKEY_CURRENT_USER,regPath,0,KEY_SET_VALUE,&hKey)!=ERROR_SUCCESS)
		return false;
	bool ok=false;
	if(enable)
	{
		wchar_t exePath[MAX_PATH]={};
		GetModuleFileNameW(nullptr,exePath,MAX_PATH);
		std::wstring quoted=L"\""+std::wstring(exePath)+L"\"";
		ok=RegSetValueExW(hKey,valueName,0,REG_SZ,
			(const BYTE*)quoted.c_str(),
			(DWORD)((quoted.length()+1)*sizeof(wchar_t)))==ERROR_SUCCESS;
	}
	else
	{
		RegDeleteValueW(hKey,valueName);
		ok=true;
	}
	RegCloseKey(hKey);
	return ok;
}

// ============================== 入口 ================================
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int)
{
	HANDLE hMutex=CreateMutexW(nullptr,TRUE,L"Global\\MyCampusAutoLoginMutex");
	if(GetLastError()==ERROR_ALREADY_EXISTS) return 0;

	srand((unsigned)GetTickCount());

	WSADATA wsa;
	if(WSAStartup(MAKEWORD(2,2),&wsa)!=0) return 1;

	WriteLog("===== 程序启动 =====");
	SetAutoStart(true);
	WriteLog("已设置开机自启动。");

	if(FAILED(CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED)))
	{
		WriteLog("COM 初始化失败。");
		WSACleanup();
		return 1;
	}

	INetworkListManager* pNLM=nullptr;
	if(FAILED(CoCreateInstance(CLSID_NetworkListManager,nullptr,CLSCTX_ALL,
		IID_INetworkListManager,(void**)&pNLM)))
	{
		WriteLog("创建 NLM 失败。");
		CoUninitialize(); WSACleanup(); return 1;
	}

	IConnectionPointContainer* pCPC=nullptr;
	IConnectionPoint* pCP=nullptr;
	DWORD cookie=0;
	auto pSink=new NetworkEventSink();

	if(SUCCEEDED(pNLM->QueryInterface(IID_IConnectionPointContainer,(void**)&pCPC)))
	{
		if(SUCCEEDED(pCPC->FindConnectionPoint(IID_INetworkConnectionEvents,&pCP)))
		{
			pCP->Advise(pSink,&cookie);
		}
	}

	CheckAndAct();  // 启动时立即检查一次

	MSG msg;
	while(GetMessage(&msg,nullptr,0,0))
	{
		if(msg.message==WM_TIMER&&msg.wParam==RETRY_TIMER_ID)
		{
			WriteLog("定时重试触发。");
			CheckAndAct();
			continue;               // 线程定时器消息无需 Dispatch
		}
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	EnsureRetryTimer(false);   // 退出前停掉定时器
	WriteLog("===== 程序退出 =====");
	if(cookie) pCP->Unadvise(cookie);
	if(pCP)    pCP->Release();
	if(pCPC)   pCPC->Release();
	pNLM->Release();
	pSink->Release();
	CoUninitialize();
	WSACleanup();
	ReleaseMutex(hMutex);
	CloseHandle(hMutex);
	return 0;
}