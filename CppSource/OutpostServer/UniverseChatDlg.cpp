// Author: Giperion.

#include "StdAfx.h"
#include "UniverseChatDlg.h"
#include "ChatRdl.h"
#include "RichEditText.h"
#include "OutpostServer.h"
#include "server_iserverimpl.h" // Server, g_pUniverseChatHook
#include "common_text.h"        // StyleNode, TextNode, BinaryRDLWriter

namespace
{
	// — the encoded outgoing message (0x800 bytes).
	char GChatUniverseBuffer[0x800];

	// — the shared chat-id header's "universe" id: word 0 and byte 2 = 1 set by
	// the CRT initializer module storage, byte 3 left at its .data zero. The target of every
	// message this dialog submits; the sender is a zero id built on the stack (0041849b).
	const CHAT_ID GChatIdUniverse(0x00010000u);

	// Style id of the outgoing message's StyleNode (module storage).
	const unsigned short ChatUniverseStyleId = 0xd681;
	const int InputLimitChars = 0x200;
	const int InputTextBytes = 0x800;
}

// — constructed by the CRT initializer module storage (ctor with a null parent),
// destroyed by its atexit thunk 004180b0.
CUniverseChatDlg GUniverseChatDlg;


CUniverseChatDlg::CUniverseChatDlg(CWnd* Parent)
	: CDialog(IDD, Parent)
{
}


void CUniverseChatDlg::DoDataExchange(CDataExchange* DX)
{
	DDX_Control(DX, 1047, InputEdit);
	DDX_Control(DX, 1048, LogEdit);
}

// Message map module storage.
BEGIN_MESSAGE_MAP(CUniverseChatDlg, CDialog)
	ON_NOTIFY(EN_MSGFILTER, 1047, &CUniverseChatDlg::OnMsgFilterInput)
	ON_BN_CLICKED(1053, &CUniverseChatDlg::OnCloseButton)
	ON_WM_CLOSE()
	ON_WM_DESTROY()
END_MESSAGE_MAP()


BOOL CUniverseChatDlg::OnInitDialog()
{
	CDialog::OnInitDialog();
	::SendMessageA(InputEdit.m_hWnd, EM_SETEVENTMASK, 0, ENM_KEYEVENTS);
	::SendMessageA(InputEdit.m_hWnd, EM_EXLIMITTEXT, 0, InputLimitChars);
	LockServer();
	g_pUniverseChatHook = &CUniverseChatDlg::OnUniverseChat;
	UnlockServer();
	return TRUE;
}

// — empty: Enter in the input box must not close the window.
void CUniverseChatDlg::OnOK()
{
}

// — JMP [vtable +0x60]: DestroyWindow only (the object is a global).
void CUniverseChatDlg::OnCancel()
{
	DestroyWindow();
}

// — JMP [vtable +0x60], DestroyWindow.
void CUniverseChatDlg::OnCloseButton()
{
	DestroyWindow();
}

// — DestroyWindow (vtable +0x60), then JMP CWnd::Default.
void CUniverseChatDlg::OnClose()
{
	DestroyWindow();
	Default();
}


void CUniverseChatDlg::OnDestroy()
{
	CWnd::OnDestroy();
	g_pUniverseChatHook = nullptr;
}

// — as CChatAccountDlg::OnMsgFilterInput, with StyleNode(0xd681), sender 0,
// target the "universe" id, system -1.
void CUniverseChatDlg::OnMsgFilterInput(NMHDR* Header, LRESULT* Result)
{
	const MSGFILTER* Filter = (const MSGFILTER*)Header;
	if (Filter->wParam != VK_RETURN)
	{
		*Result = 0;
		return;
	}

	if (::SendMessageA(InputEdit.m_hWnd, WM_GETTEXTLENGTH, 0, 0) != 0)
	{
		wchar_t Text[InputTextBytes / sizeof(wchar_t)];
		GetRichEditText(InputEdit.m_hWnd, Text, InputTextBytes);
		LockServer();
		{
			RenderDisplayList Rdl;
			// Double clone as in CChatAccountDlg::OnMsgFilterInput: holder of the first clone
			// (0041838a..00418396), list insert 004149a0 (second clone), holder delete
			// (004183bd..004183d3), ~StyleNode (004183e2); the text at 004183e8..00418451.
			{
				StyleNode Style(ChatUniverseStyleId);
				{
					const FChatRdlNodeHolder FirstClone(Style.Clone());
					AppendChatRdlNode(Rdl, FirstClone);
				}
			}
			{
				TextNode Line(Text, -1);
				{
					const FChatRdlNodeHolder FirstClone(Line.Clone());
					AppendChatRdlNode(Rdl, FirstClone);
				}
			}

			BinaryRDLWriter Writer;
			unsigned int Size;
			if (Writer.write_buffer(Rdl, GChatUniverseBuffer, sizeof(GChatUniverseBuffer), Size))
			{
				const CHAT_ID Sender(0u);
				Server.SubmitChat(Sender, Size, GChatUniverseBuffer, GChatIdUniverse, -1);
			}
			// baseline releases the server lock before the list is destroyed (004184b6).
			UnlockServer();
		}
	}

	InputEdit.SetSel(0, -1);
	::SendMessageA(InputEdit.m_hWnd, WM_CLEAR, 0, 0);
	*Result = 1;
}

// — writes to the global window's log with no window test (the hook is only
// installed while the window exists).
void __cdecl CUniverseChatDlg::OnUniverseChat(wchar_t* Text, int PlayerId)
{
	(void)PlayerId;
	LockServer();
	const HWND Log = GUniverseChatDlg.LogEdit.m_hWnd;
	const int LinesBefore = (int)::SendMessageA(Log, EM_GETLINECOUNT, 0, 0);
	AppendRichEditText(Log, Text);
	AppendRichEditText(Log, L"\n");
	const int LinesAfter = (int)::SendMessageA(GUniverseChatDlg.LogEdit.m_hWnd, EM_GETLINECOUNT, 0, 0);
	if (::GetFocus() != GUniverseChatDlg.LogEdit.m_hWnd)
	{
		GUniverseChatDlg.LogEdit.LineScroll(LinesAfter - LinesBefore, 0);
	}
	UnlockServer();
}
