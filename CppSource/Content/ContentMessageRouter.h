// Author: Giperion.

#pragma once

// Content.dll TU36 (06f056d0-06f05d00): the StateMachine message router that every
// Content MessageHandler / StateMachineMessageHandler is bound to. GameSupport creates
// the single instance (the routine: operator new(0x24) + the routine) and deletes it
// in its dtor (the routine: the field->vtbl[+0x10](1)).

#include "common_net.h"
#include "common_server_controller.h"

#include <list>

class FContentMessageRouter;

// the live router, or null.
extern FContentMessageRouter* GMessageRouter;

// baseline abstract base: vtable 06fb80d4 holds only the four IMessageRouter slots (all
// purecall). Its ctor (inlined at the top of the routine) publishes the object in
// ; its dtor the routine (also run from the ctor's unwind funclets
// / 06facd52) clears it.
class FContentMessageRouterBase : public IMessageRouter
{
protected:
    FContentMessageRouterBase();
    ~FContentMessageRouterBase();
};

// baseline object: 0x24 bytes, vtable 06fb80bc (6 slots).
//   +0x04 pending delayed messages (VC6 list: proxy +4, head +8, size +0xc), ordered by
//         delivery time
//   +0x10 timer id (the field++ at construction)
//   +0x14 last handler id handed out
//   +0x18 registered handlers (VC6 list: proxy +0x18, head +0x1c, size +0x20)
class FContentMessageRouter : public FContentMessageRouterBase
{
public:
    // .
    FContentMessageRouter();

    // Slot 0, the branch (RET 0x14).
    void RouteMessage(StateMachineMessage* Msg, float Delay, unsigned int Target,
                      unsigned int Sender, int Scope) override;
    // Slot 1, the routine (RET 4).
    unsigned int RegisterHandler(MessageHandler* Handler) override;
    // Slot 2, the routine (RET 4).
    void UnregisterHandler(MessageHandler* Handler) override;
    // Slot 3, the routine (RET 4).
    MessageHandler* LookupHandlerById(unsigned int Id) override;
    // Slot 4, the routine scalar deleting dtor -> the routine.
    virtual ~FContentMessageRouter();
    // Slot 5, the branch (RET 4): SpaceSupport forwards its timer message here
    // (06f5d74f), for the timers RouteMessage sets on the field.
    virtual void OnTimerExpired(pub::Controller::TimerExpired const* Timer);

private:
    std::list<StateMachineMessage*> PendingMessages;   // +0x04
    unsigned int TimerId;                               // +0x10
    unsigned int LastHandlerId;                         // +0x14
    std::list<MessageHandler*> Handlers;                // +0x18
};
