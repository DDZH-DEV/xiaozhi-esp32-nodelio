#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Owns no text or timers. The caller keeps the chunk alive and serializes access.
class NewsReadProgress {
public:
    void Begin(std::string_view chunk, size_t offset, bool more) {
        if (++generation_ == 0) {
            ++generation_;
        }
        chunk_ = chunk;
        offset_ = offset;
        matched_end_ = 0;
        matched_chars_ = 0;
        more_ = more;
        active_ = !chunk.empty() && chunk.size() <= 640;
        complete_ = false;
    }

    void Clear() {
        Cancel();
        chunk_ = {};
        complete_ = false;
    }
    void Cancel() { active_ = false; }
    uint32_t token() const { return active_ ? generation_ : 0; }
    bool waiting() const { return active_ && !complete_; }
    bool unfinished() const { return !chunk_.empty() && !complete_; }

    // Align subtitle text to this chunk, ignoring punctuation and whitespace.
    // Also tolerates a spoken title/preamble. No allocations on the UI path.
    bool Sentence(std::string_view sentence, size_t& byte_offset) {
        if (!active_ || sentence.empty() || sentence.size() > 4096) {
            return false;
        }
        // Longest common substring over significant Unicode characters. One bounded
        // DP row keeps matching O(chunk_chars * sentence_chars), even for repeats.
        uint16_t lengths[1025] = {};
        size_t best_count = 0, best_start_char = 0, best_end = 0, chars = 0, bi = 0;
        size_t sentence_chars = 0;
        while (const auto bc = Next(chunk_, bi)) {
            ++chars;
            size_t si = 0, j = 0;
            uint16_t diagonal = 0;
            while (const auto sc = Next(sentence, si)) {
                if (++j > 1024) {
                    return false;
                }
                const uint16_t previous = lengths[j];
                lengths[j] = bc == sc ? diagonal + 1 : 0;
                diagonal = previous;
                if (lengths[j] > best_count) {
                    best_count = lengths[j];
                    best_start_char = chars - best_count;
                    best_end = bi;
                }
            }
            sentence_chars = j;
        }
        size_t best_start = 0;
        for (size_t n = 0; n < best_start_char; ++n) {
            Next(chunk_, best_start);
        }
        // Avoid aligning incidental words such as "好的" to a long article.
        size_t minimum = chars < 4 ? chars : 4;
        if (sentence_chars < minimum) {
            minimum = sentence_chars;
        }
        const bool short_tail = matched_chars_ > 0 && best_start_char == matched_chars_ &&
                                best_start_char + best_count == chars;
        if (!minimum || (best_count < minimum && !short_tail) || best_end <= matched_end_ ||
            best_start_char > matched_chars_) {
            return false;
        }
        matched_end_ = best_end;
        matched_chars_ = best_start_char + best_count;
        byte_offset = offset_ + best_start;
        return true;
    }

    // Only the application may finish, after TTS stop AND actual playback drain.
    bool Finish(uint32_t token, bool played_audio, bool& more) {
        more = false;
        if (!token || token != this->token() || !played_audio || !matched_end_) {
            return false;
        }
        size_t remaining = matched_end_;
        if (Next(chunk_, remaining)) {
            return false;  // Never skip text after a summary or truncated response.
        }
        complete_ = true;
        active_ = false;
        more = more_;
        return true;
    }

private:
    static uint32_t Next(std::string_view text, size_t& i) {
        while (i < text.size()) {
            uint32_t cp = static_cast<unsigned char>(text[i++]);
            unsigned continuation = cp < 0x80 ? 0 : cp < 0xe0 ? 1 : cp < 0xf0 ? 2 : 3;
            if (continuation) {
                cp &= (1u << (6 - continuation)) - 1;
                while (continuation-- && i < text.size()) {
                    cp = (cp << 6) | (static_cast<unsigned char>(text[i++]) & 0x3f);
                }
            }
            if (cp <= 0x20 || (cp >= 0x21 && cp <= 0x2f) || (cp >= 0x3a && cp <= 0x40) ||
                (cp >= 0x5b && cp <= 0x60) || (cp >= 0x7b && cp <= 0x7e) ||
                (cp >= 0x2000 && cp <= 0x206f) || (cp >= 0x3000 && cp <= 0x303f) || cp == 0xff0c ||
                cp == 0xff01 || cp == 0xff1f || cp == 0xff1a || cp == 0xff1b) {
                continue;
            }
            return cp >= 'A' && cp <= 'Z' ? cp + ('a' - 'A') : cp;
        }
        return 0;
    }

    std::string_view chunk_;
    uint32_t generation_ = 0;
    size_t offset_ = 0;
    size_t matched_end_ = 0;
    size_t matched_chars_ = 0;
    bool more_ = false;
    bool active_ = false;
    bool complete_ = false;
};
