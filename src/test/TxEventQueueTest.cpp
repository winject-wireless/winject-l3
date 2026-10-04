#include "radio/TxEvent.h"

#include <gtest/gtest.h>
#include <netinet/in.h>

using namespace winject;

TEST(TxEventQueueTest, PushDataDedupes)
{
    TxEventQueue q;
    q.push_data();
    q.push_data();
    TxEvent ev;
    EXPECT_TRUE(q.pop(&ev));
    EXPECT_TRUE(std::holds_alternative<EventData>(ev));
    EXPECT_FALSE(q.pop(&ev));
}

TEST(TxEventQueueTest, DataCtrlDataOrdering)
{
    TxEventQueue q;
    q.push_data();
    EventCtrlSendSocketChange ctrl;
    ctrl.dplane.sin_port = htons(1);
    q.push(ctrl);
    q.push_data();
    TxEvent ev;
    EXPECT_TRUE(q.pop(&ev));
    EXPECT_TRUE(std::holds_alternative<EventData>(ev));
    EXPECT_TRUE(q.pop(&ev));
    EXPECT_TRUE(std::holds_alternative<EventCtrlSendSocketChange>(ev));
    EXPECT_TRUE(q.pop(&ev));
    EXPECT_TRUE(std::holds_alternative<EventData>(ev));
}

TEST(TxEventQueueTest, TwoCtrlEventsKeptInOrder)
{
    TxEventQueue q;
    EventCtrlSendSocketChange a;
    a.dplane.sin_port = htons(1);
    EventCtrlSendSocketChange b;
    b.dplane.sin_port = htons(2);
    q.push(a);
    q.push(b);
    TxEvent ev;
    EXPECT_TRUE(q.pop(&ev));
    EXPECT_EQ(ntohs(std::get<EventCtrlSendSocketChange>(ev).dplane.sin_port),
              1);
    EXPECT_TRUE(q.pop(&ev));
    EXPECT_EQ(ntohs(std::get<EventCtrlSendSocketChange>(ev).dplane.sin_port),
              2);
}

TEST(TxEventQueueTest, DropDataKeepsCtrl)
{
    TxEventQueue q;
    q.push_data();
    EventCtrlSendSocketChange c1;
    q.push(c1);
    q.push_data();
    EventCtrlSendSocketChange c2;
    q.push(c2);
    q.drop_data();
    TxEvent ev;
    EXPECT_TRUE(q.pop(&ev));
    EXPECT_TRUE(std::holds_alternative<EventCtrlSendSocketChange>(ev));
    EXPECT_TRUE(q.pop(&ev));
    EXPECT_TRUE(std::holds_alternative<EventCtrlSendSocketChange>(ev));
    EXPECT_FALSE(q.pop(&ev));
}
