#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>
#include "device_protocol_limits.h"

// Main-task-only text projection; it never controls capture or playback.
class EzDialogue {
public:
    void Begin(uint64_t capture) {
        if (!pending_)
            Clear();
        capture_ = capture;
        pending_ = true;
    }

    void Register(uint64_t capture, const std::string& id) {
        if (capture != capture_ || id.empty() || FindInput(id) != inputs_.end() ||
            inputs_.size() == 16)
            return;
        inputs_.push_back({id, ""});
    }

    bool Transcript(const std::string& id, const std::string& text) {
        auto input = FindInput(id);
        if (input == inputs_.end() || input->text == text)
            return false;
        size_t used = 0;
        for (const auto& part : inputs_)
            if (part.id != id)
                used += part.text.size();
        input->text = Prefix(text, DEVICE_MEDIA_TEXT_MAX_BYTES - used);
        question_.clear();
        for (const auto& part : inputs_)
            Append(question_, part.text);
        return true;
    }

    void Accepted(const std::string& id, const std::string& run) {
        if (FindInput(id) == inputs_.end()) {
            if (Contains(retired_inputs_, id))
                Remember(retired_runs_, run);
            return;
        }
        run_ = run;
        pending_ = false;
    }

    bool Output(const std::string& run, const std::string& id, const std::string& text) {
        if (Contains(retired_runs_, run))
            return false;
        if (run != run_) {
            if (pending_)
                return false;
            Clear();
            run_ = run;  // A managed output has no local question to attach to it.
        }
        if (Contains(outputs_, id))
            return false;
        Remember(outputs_, id);
        Append(answer_, text);
        return true;
    }

    void CancelRun(const std::string& run) {
        Remember(retired_runs_, run);
        if (run_ == run)
            Clear();
    }

    void RejectInput(const std::string& id) {
        if (pending_ && FindInput(id) != inputs_.end())
            Clear();
    }

    void Clear() {
        for (const auto& input : inputs_)
            Remember(retired_inputs_, input.id);
        if (!run_.empty())
            Remember(retired_runs_, run_);
        inputs_.clear();
        outputs_.clear();
        run_.clear();
        question_.clear();
        answer_.clear();
        capture_ = 0;
        pending_ = false;
    }

    const std::string& Question() const { return question_; }
    const std::string& Answer() const { return answer_; }

private:
    struct Input {
        std::string id, text;
    };
    uint64_t capture_ = 0;
    bool pending_ = false;
    std::vector<Input> inputs_;
    std::vector<std::string> retired_inputs_, retired_runs_, outputs_;
    std::string run_, question_, answer_;

    std::vector<Input>::iterator FindInput(const std::string& id) {
        return std::find_if(inputs_.begin(), inputs_.end(),
                            [&id](const Input& input) { return input.id == id; });
    }
    static bool Contains(const std::vector<std::string>& ids, const std::string& id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    }
    static void Remember(std::vector<std::string>& ids, const std::string& id) {
        if (id.empty() || Contains(ids, id))
            return;
        if (ids.size() == 32)
            ids.erase(ids.begin());
        ids.push_back(id);
    }
    static std::string Prefix(const std::string& text, size_t size) {
        if (text.size() <= size)
            return text;
        if (size < 3)
            return "";
        size -= 3;
        while (size && (static_cast<unsigned char>(text[size]) & 0xc0) == 0x80)
            --size;
        return text.substr(0, size) + "…";
    }
    static void Append(std::string& target, const std::string& text) {
        if (text.empty())
            return;
        if (!target.empty())
            target += '\n';
        target = Prefix(target + text, DEVICE_MEDIA_TEXT_MAX_BYTES);
    }
};
