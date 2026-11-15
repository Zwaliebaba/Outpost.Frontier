// Author: Giperion.

#include "CCaret.h"
#include "ImeUiCore.h"
#include <cstring>

CCaret::CCaret(HWND HWnd)
{
    m_bShow = false;
    BlinkTimestamp = 0; // baseline +4 stores blink timestamp, ignores HWnd.
    m_bCaretState = false;
    m_nX = 0;
    m_nY = 0;
}

CCaret::~CCaret()
{
}

CCaret& CCaret::operator=(const CCaret& Other)
{
    memcpy(this, &Other, sizeof(CCaret));
    return *this;
}

void CCaret::Show(bool bShow)
{
    m_bShow = bShow;
}

bool CCaret::SetPos(int X, int Y)
{
    m_nX = X;
    m_nY = Y;
    ImeUi_SetCaretPosition(X, Y);
    return true;
}

bool CCaret::UpdateCaret()
{
    if (!ImeUi_IsCompositionEmpty() || !m_bShow) return false;
    if (m_bBlink)
    {
        DWORD CurrentTime = GetTickCount();
        if (CurrentTime < BlinkTimestamp) BlinkTimestamp = 0;
        if (GetCaretBlinkTime() < CurrentTime - BlinkTimestamp)
        {
            BlinkTimestamp = CurrentTime;
            m_bCaretState = !m_bCaretState;
        }
    }
    return m_bCaretState;
}

void CCaret::SetBlink(bool bBlink)
{
    m_bCaretState = true;
    m_bBlink = bBlink;
}
