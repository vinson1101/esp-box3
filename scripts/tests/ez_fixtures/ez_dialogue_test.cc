#include "ez_dialogue.h"
#include <cassert>
#include <string>
int main() {
    EzDialogue d;
    d.Begin(1);
    d.Register(9, "wrong");
    assert(!d.Transcript("wrong", "old"));
    d.Register(1, "a");
    d.Begin(2);
    d.Register(2, "b");
    assert(d.Transcript("b", "后半句"));
    assert(d.Transcript("a", "前半句"));
    assert(d.Question() == "前半句\n后半句");
    assert(!d.Transcript("a", "前半句"));
    assert(!d.Output("unbound", "o", "too early"));
    d.Accepted("a", "run-a");
    assert(d.Output("run-a", "o-1", "第一段"));
    assert(!d.Output("run-a", "o-1", "重复内容"));
    assert(d.Output("run-a", "o-2", "第二段"));
    assert(d.Answer() == "第一段\n第二段");
    d.RejectInput("a");
    assert(!d.Answer().empty());
    d.Begin(3);
    d.Register(3, "c");
    assert(d.Question().empty() && d.Answer().empty());
    assert(!d.Output("run-a", "late", "old"));
    d.Accepted("c", "run-c");
    d.CancelRun("run-c");
    assert(!d.Output("run-c", "cancelled", "old"));
    assert(d.Output("managed", "managed-1", "主动输出"));
    assert(d.Question().empty() && d.Answer() == "主动输出");
    d.Begin(4);
    d.Register(4, "d");
    d.Clear();
    d.Accepted("d", "late-run");
    assert(!d.Output("late-run", "late", "old"));
    d.Begin(5);
    d.Register(5, "empty");
    d.RejectInput("empty");
    assert(!d.Transcript("empty", "late ASR"));
    std::string long_text;
    for (int i = 0; i < 20000; ++i)
        long_text += "中";
    assert(d.Output("large", "large-1", long_text));
    assert(d.Answer().size() <= DEVICE_MEDIA_TEXT_MAX_BYTES);
    assert(d.Answer().substr(d.Answer().size() - 3) == "…");
    assert(d.Answer().size() % 3 == 0);
    d.Clear();
    assert(d.Question().empty() && d.Answer().empty());
}
