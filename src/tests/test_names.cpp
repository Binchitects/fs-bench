// File names: Unicode, special characters, length limits, case sensitivity.
#include <fstream>

#include "test_util.hpp"

namespace fsb {
using namespace tu;

FSB_TEST(names, unicode, "Unicode names in many scripts round-trip byte-exact through create/list/open") {
    const std::vector<std::string> names = {
        "caf\xC3\xA9",                                        // café (precomposed)
        "cafe\xCC\x81",                                       // café (decomposed)
        "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E",               // 日本語
        "\xCE\x95\xCE\xBB\xCE\xBB\xCE\xB7\xCE\xBD\xCE\xB9\xCE\xBA\xCE\xAC",  // Ελληνικά
        "\xD1\x80\xD1\x83\xD1\x81\xD1\x81\xD0\xBA\xD0\xB8\xD0\xB9",          // русский
        "\xD7\xA2\xD7\x91\xD7\xA8\xD7\x99\xD7\xAA",           // עברית
        "\xD9\x81\xD8\xA7\xD8\xB1\xD8\xB3\xDB\x8C",           // فارسی
        "emoji_\xF0\x9F\x98\x80\xF0\x9F\x9A\x80",             // 😀🚀 (outside the BMP)
        "zero\xE2\x80\x8Bwidth",                              // zero-width space
        "\xED\x95\x9C\xEA\xB5\xAD\xEC\x96\xB4",               // 한국어
    };
    int normalized = 0;
    for (auto& n : names) {
        auto ec = capture_error([&] { write_file(ctx.path(n), n); });
        CHECK_MSG(!ec, "cannot create '", n, "': ", ec.message());
    }
    auto listed = list_names(ctx.root);
    for (auto& n : names) {
        if (!listed.count(n)) {
            ++normalized;
            ctx.note("'" + n + "' is listed under a different (normalized) form");
            continue;
        }
        CHECK_MSG(read_file(ctx.path(n)) == n, "content mismatch for '", n, "'");
    }
    // Precomposed and decomposed café are distinct names unless the fs normalizes.
    ctx.feature("Unicode normalization", listed.size() == names.size() && !normalized ? "none (byte-exact)"
                                                                                       : "normalizing");
    CHECK_MSG(listed.size() + 1 >= names.size(), "lost entries: created ", names.size(), ", listed ", listed.size());
}

FSB_TEST(names, special_chars, "names with spaces, dots, shell metacharacters and leading dashes") {
    const std::vector<std::string> names = {" leading space", "trailing space ", "trailing.dot.", "-dash", "#hash",
                                            "%percent", "&amp", "{brace}", "$dollar", "!bang", "'quote'", "@at",
                                            "+plus", "`tick`", "=eq", "[bracket]", ";semi", ",comma", "~tilde",
                                            ".hidden", "..dots", "a b\tc", "multiple...dots"};
    int ok = 0;
    std::string rejected;
    for (auto& n : names) {
        auto ec = capture_error([&] { write_file(ctx.path(n), n); });
        if (ec) {
            rejected += " [" + n + "]";
            continue;
        }
        ++ok;
        CHECK_MSG(list_names(ctx.root).count(n), "'", n, "' created but listed differently");
        CHECK_MSG(read_file(ctx.path(n)) == n, "content mismatch for '", n, "'");
    }
    ctx.feature("Special-character names", cat(ok, "/", names.size(), " accepted"));
    if (!rejected.empty()) ctx.note("rejected:" + rejected);
}

FSB_TEST(names, reserved_chars, "which of \\ : * ? \" < > | and control characters are allowed") {
    std::vector<std::pair<std::string, std::string>> cases = {
        {"backslash", "a\\b"}, {"asterisk", "a*b"}, {"question", "a?b"}, {"dquote", "a\"b"},
        {"lt", "a<b"},         {"gt", "a>b"},       {"pipe", "a|b"},     {"ctrl-01", "a\x01" "b"},
    };
#ifndef _WIN32
    cases.push_back({"colon", "a:b"});  // on Windows ':' selects an alternate data stream
#endif
    std::string allowed, refused;
    for (auto& [label, n] : cases) {
        auto ec = capture_error([&] { write_file(ctx.root / u8p(n), "x"); });
        bool exact = !ec && list_names(ctx.root).count(n);
        (exact ? allowed : refused) += " " + label;
        std::error_code ignore;
        if (!ec) fs::remove(ctx.root / u8p(n), ignore);
    }
    ctx.feature("Windows-reserved chars allowed", allowed.empty() ? "none" : allowed.substr(1));
    if (!refused.empty()) ctx.note("refused:" + refused);
}

FSB_TEST(names, reserved_device_names, "DOS device names (CON, NUL, AUX, COM1, LPT1) as file names") {
    std::string usable, not_usable;
    for (std::string n : {"CON", "nul.txt", "AUX", "COM1", "LPT1.log"}) {
        auto p = ctx.root / n;
        {
            std::ofstream o(p);  // go through the normal (non-\\?\) path translation
            if (o) o << "x";
        }
        bool real = list_names(ctx.root).count(n) > 0;
        (real ? usable : not_usable) += " " + n;
    }
    ctx.feature("DOS device names usable", usable.empty() ? "no" : usable.substr(1));
    // Clean up via plain names first; leftovers are removed by the runner.
    for (auto& e : fs::directory_iterator(ctx.root)) {
        std::error_code ec;
        fs::remove(e.path(), ec);
    }
}

FSB_TEST(names, max_name_length, "maximum file name length in bytes and in multi-byte characters") {
    auto try_name = [&](const std::string& n) {
        auto ec = capture_error([&] { touch(ctx.root / u8p(n)); });
        if (!ec) {
            std::error_code ignore;
            fs::remove(ctx.root / u8p(n), ignore);
        }
        return !ec;
    };
    auto repeat = [](const std::string& unit, size_t n) {
        std::string s;
        for (size_t i = 0; i < n; ++i) s += unit;
        return s;
    };
    auto max_units = [&](const std::string& unit) {
        size_t lo = 0, hi = 1024;  // largest count that works
        while (lo < hi) {
            size_t mid = (lo + hi + 1) / 2;
            if (try_name(repeat(unit, mid))) lo = mid;
            else hi = mid - 1;
        }
        return lo;
    };
    size_t ascii = max_units("a");
    size_t two = max_units("\xC3\xA9");             // é: 2 UTF-8 bytes, 1 UTF-16 unit
    size_t four = max_units("\xF0\x9F\x98\x80");    // 😀: 4 UTF-8 bytes, 2 UTF-16 units
    ctx.feature("Max name: ASCII chars", std::to_string(ascii));
    ctx.feature("Max name: 'é' chars", std::to_string(two));
    ctx.feature("Max name: emoji chars", std::to_string(four));
    CHECK_MSG(ascii >= 255, "only ", ascii, " ASCII characters allowed");
    auto ec = capture_error([&] { touch(ctx.root / std::string(ascii + 1, 'a')); });
    auto k = plat::classify(ec);
    CHECK_MSG(k == ErrKind::NameTooLong || k == ErrKind::InvalidArg || k == ErrKind::NotFound,
              "over-long name gave ", plat::err_kind_name(k));
}

FSB_TEST(names, case_sensitivity, "case sensitivity and case preservation of names") {
    write_file(ctx.path("CaseTest.txt"), "upper");
    bool insensitive = fs::exists(ctx.path("casetest.txt"));
    bool preserving = list_names(ctx.root).count("CaseTest.txt") == 1;
    ctx.feature("Case sensitive", insensitive ? "no" : "yes");
    ctx.feature("Case preserving", preserving ? "yes" : "no");
    if (!insensitive) {
        write_file(ctx.path("casetest.txt"), "lower");
        CHECK_EQ(list_names(ctx.root).size(), 2u);
        CHECK(read_file(ctx.path("CaseTest.txt")) == "upper");
        CHECK(read_file(ctx.path("casetest.txt")) == "lower");
    } else {
        CHECK(read_file(ctx.path("CASETEST.TXT")) == "upper");
        // Exclusive create under a different case must collide.
        CHECK_ERR(File::open(ctx.path("CASETEST.txt"), write | create | exclusive), Exists);
    }
}

FSB_TEST(names, case_only_rename, "renaming a file to a different case of the same name") {
    write_file(ctx.path("lower.txt"), "content");
    plat::rename_replace(ctx.path("lower.txt"), ctx.path("LOWER.txt"));
    auto names = list_names(ctx.root);
    CHECK_MSG(names.count("LOWER.txt") == 1 && names.size() == 1, "listing after rename has ", names.size(),
              " entries");
    CHECK(read_file(ctx.path("LOWER.txt")) == "content");
}

}  // namespace fsb
