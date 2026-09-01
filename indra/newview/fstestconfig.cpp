/**
 * @file fstestconfig.cpp
 * @brief Config-file loading for the automated test harness (FSTestHarness).
 *
 * $LicenseInfo:firstyear=2025&license=fsviewerlgpl$
 * Phoenix Firestorm Viewer Source Code
 * Copyright (C) 2026, The Phoenix Firestorm Project, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation;
 * version 2.1 of the License only.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 *
 * The Phoenix Firestorm Project, Inc., 1831 Oakwood Drive, Fairmont, Minnesota 56031-3225 USA
 * http://www.firestormviewer.org
 * $/LicenseInfo$
 */

#include "llviewerprecompiledheaders.h"

#include "fstestconfig.h"

#include <fstream>
#include <sstream>
#include <vector>

#include "llstring.h"

namespace
{
    // ---------------------------------------------------------------------
    // Small helpers
    // ---------------------------------------------------------------------

    std::string trim(const std::string& s)
    {
        const std::string ws(" \t\r\n");
        const size_t b = s.find_first_not_of(ws);
        if (b == std::string::npos)
        {
            return std::string();
        }
        const size_t e = s.find_last_not_of(ws);
        return s.substr(b, e - b + 1);
    }

    std::string lineError(size_t line_no, const std::string& what)
    {
        std::ostringstream out;
        out << "line " << line_no << ": " << what;
        return out.str();
    }

    bool isBareKeyChar(char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-';
    }

    /**
     * Split a table header's dotted path ("avatars.primary") into segments.
     * Quoted segments are not supported -- they are rejected by the caller.
     */
    bool splitDotted(const std::string& raw, std::vector<std::string>& out, std::string& why)
    {
        out.clear();
        std::string current;
        for (char c : raw)
        {
            if (c == '.')
            {
                if (current.empty())
                {
                    why = "empty path segment in '" + raw + "'";
                    return false;
                }
                out.push_back(current);
                current.clear();
            }
            else if (isBareKeyChar(c))
            {
                current += c;
            }
            else
            {
                why = "unsupported character '" + std::string(1, c)
                    + "' in table name '" + raw + "' (bare keys only)";
                return false;
            }
        }
        if (current.empty())
        {
            why = "empty path segment in '" + raw + "'";
            return false;
        }
        out.push_back(current);
        return true;
    }

    /**
     * Parse a TOML basic string ("...", backslash escapes) or literal string
     * ('...', no escapes) starting at text[pos] == quote char. On success pos
     * is advanced past the closing quote.
     */
    bool parseQuoted(const std::string& text, size_t& pos, std::string& out, std::string& why)
    {
        const char quote = text[pos];
        const bool literal = (quote == '\'');
        ++pos;
        out.clear();
        while (pos < text.size())
        {
            const char c = text[pos];
            if (c == quote)
            {
                ++pos;
                return true;
            }
            if (c == '\n')
            {
                break; // unterminated; multi-line strings are not supported
            }
            if (!literal && c == '\\')
            {
                ++pos;
                if (pos >= text.size())
                {
                    break;
                }
                const char esc = text[pos];
                switch (esc)
                {
                    case '"':  out += '"';  break;
                    case '\\': out += '\\'; break;
                    case 'n':  out += '\n'; break;
                    case 't':  out += '\t'; break;
                    case 'r':  out += '\r'; break;
                    case 'b':  out += '\b'; break;
                    case 'f':  out += '\f'; break;
                    case '0':  out += '\0'; break;
                    default:
                        // \u / \U and anything else: refuse rather than guess.
                        why = std::string("unsupported string escape '\\")
                            + esc + "'";
                        return false;
                }
                ++pos;
                continue;
            }
            out += c;
            ++pos;
        }
        why = "unterminated string";
        return false;
    }

    /**
     * Parse the value half of "key = value". Accepts a quoted string, an
     * integer (with optional sign and TOML's '_' separators) or a boolean.
     * Anything else is refused by name.
     */
    bool parseValue(const std::string& raw_in, LLSD& out, std::string& why)
    {
        const std::string raw = trim(raw_in);
        if (raw.empty())
        {
            why = "missing value";
            return false;
        }

        if (raw[0] == '"' || raw[0] == '\'')
        {
            size_t pos = 0;
            std::string s;
            if (!parseQuoted(raw, pos, s, why))
            {
                return false;
            }
            // Only a comment may follow the closing quote.
            const std::string rest = trim(raw.substr(pos));
            if (!rest.empty() && rest[0] != '#')
            {
                why = "trailing text after string value: '" + rest + "'";
                return false;
            }
            out = s;
            return true;
        }

        // Strip a trailing comment from an unquoted value.
        std::string body = raw;
        const size_t hash = body.find('#');
        if (hash != std::string::npos)
        {
            body = trim(body.substr(0, hash));
        }

        if (body == "true")  { out = true;  return true; }
        if (body == "false") { out = false; return true; }

        // Integer.
        std::string digits;
        size_t i = 0;
        bool negative = false;
        if (i < body.size() && (body[i] == '+' || body[i] == '-'))
        {
            negative = (body[i] == '-');
            ++i;
        }
        bool any = false;
        for (; i < body.size(); ++i)
        {
            const char c = body[i];
            if (c == '_')
            {
                continue; // TOML digit separator
            }
            if (c < '0' || c > '9')
            {
                any = false;
                break;
            }
            digits += c;
            any = true;
        }
        if (any && !digits.empty())
        {
            try
            {
                const long long v = std::stoll(digits);
                out = LLSD::Integer(negative ? -v : v);
                return true;
            }
            catch (const std::exception&)
            {
                why = "integer out of range: '" + body + "'";
                return false;
            }
        }

        if (!body.empty() && (body[0] == '[' || body[0] == '{'))
        {
            why = "arrays and inline tables are not supported here: '" + body + "'";
            return false;
        }

        why = "unsupported or unquoted value: '" + body
            + "' (strings must be quoted)";
        return false;
    }

    /** Fetch a string field from an LLSD map, or "" if absent. */
    std::string mapString(const LLSD& map, const char* key)
    {
        return map.has(key) ? map[key].asString() : std::string();
    }
}

namespace FSTestConfig
{

bool parseTomlSubset(const std::string& text, LLSD& result, std::string& error)
{
    result = LLSD::emptyMap();
    error.clear();

    // The table the following key/value lines belong to, as a path. Empty =
    // the document root.
    std::vector<std::string> table_path;

    std::istringstream in(text);
    std::string raw_line;
    size_t line_no = 0;

    while (std::getline(in, raw_line))
    {
        ++line_no;
        const std::string line = trim(raw_line);
        if (line.empty() || line[0] == '#')
        {
            continue;
        }

        // Table header.
        if (line[0] == '[')
        {
            if (line.size() > 1 && line[1] == '[')
            {
                error = lineError(line_no, "arrays of tables ([[...]]) are not supported");
                return false;
            }
            const size_t close = line.find(']');
            if (close == std::string::npos)
            {
                error = lineError(line_no, "unterminated table header");
                return false;
            }
            const std::string rest = trim(line.substr(close + 1));
            if (!rest.empty() && rest[0] != '#')
            {
                error = lineError(line_no, "trailing text after table header: '" + rest + "'");
                return false;
            }
            const std::string inner = trim(line.substr(1, close - 1));
            if (inner.empty())
            {
                error = lineError(line_no, "empty table header");
                return false;
            }
            std::string why;
            if (!splitDotted(inner, table_path, why))
            {
                error = lineError(line_no, why);
                return false;
            }

            // Materialise the table so an empty one still appears.
            LLSD* node = &result;
            for (const std::string& seg : table_path)
            {
                if (!(*node).has(seg) || !(*node)[seg].isMap())
                {
                    (*node)[seg] = LLSD::emptyMap();
                }
                node = &(*node)[seg];
            }
            continue;
        }

        // key = value
        const size_t eq = line.find('=');
        if (eq == std::string::npos)
        {
            error = lineError(line_no, "expected 'key = value' or a [table] header");
            return false;
        }
        const std::string key = trim(line.substr(0, eq));
        if (key.empty())
        {
            error = lineError(line_no, "missing key before '='");
            return false;
        }
        for (char c : key)
        {
            if (!isBareKeyChar(c))
            {
                error = lineError(line_no, std::string("unsupported character '")
                                  + c + "' in key '" + key + "' (bare keys only)");
                return false;
            }
        }

        LLSD value;
        std::string why;
        if (!parseValue(line.substr(eq + 1), value, why))
        {
            error = lineError(line_no, why);
            return false;
        }

        LLSD* node = &result;
        for (const std::string& seg : table_path)
        {
            node = &(*node)[seg];
        }
        if ((*node).has(key))
        {
            error = lineError(line_no, "duplicate key '" + key + "'");
            return false;
        }
        (*node)[key] = value;
    }

    return true;
}

bool parseTomlFile(const std::string& path, LLSD& result, std::string& error)
{
    std::ifstream file(path.c_str(), std::ios::in | std::ios::binary);
    if (!file)
    {
        error = "cannot open '" + path + "'";
        return false;
    }
    std::ostringstream buffer;
    buffer << file.rdbuf();

    std::string parse_error;
    if (!parseTomlSubset(buffer.str(), result, parse_error))
    {
        error = path + ": " + parse_error;
        return false;
    }
    return true;
}

bool loadCredentials(const std::string& path,
                     const std::string& wanted_key,
                     Avatar& avatar,
                     std::string& error)
{
    LLSD doc;
    if (!parseTomlFile(path, doc, error))
    {
        return false;
    }

    if (!doc.has("avatars") || !doc["avatars"].isMap() || doc["avatars"].size() == 0)
    {
        error = path + ": no [avatars.<name>] entries";
        return false;
    }
    const LLSD& avatars = doc["avatars"];

    // Which one? explicit key, else default_avatar, else the sole entry.
    std::string key = wanted_key;
    if (key.empty() && doc.has("default_avatar"))
    {
        key = doc["default_avatar"].asString();
    }
    if (key.empty())
    {
        if (avatars.size() == 1)
        {
            key = avatars.beginMap()->first;
        }
        else
        {
            std::ostringstream out;
            out << path << ": " << avatars.size() << " avatars and no"
                << " default_avatar; pass --avatar <key>. Available:";
            for (LLSD::map_const_iterator it = avatars.beginMap();
                 it != avatars.endMap(); ++it)
            {
                out << ' ' << it->first;
            }
            error = out.str();
            return false;
        }
    }

    if (!avatars.has(key) || !avatars[key].isMap())
    {
        std::ostringstream out;
        out << path << ": no avatar '" << key << "'. Available:";
        for (LLSD::map_const_iterator it = avatars.beginMap();
             it != avatars.endMap(); ++it)
        {
            out << ' ' << it->first;
        }
        error = out.str();
        return false;
    }
    const LLSD& entry = avatars[key];

    avatar = Avatar();
    avatar.mKey        = key;
    avatar.mFirst      = mapString(entry, "first");
    avatar.mLast       = mapString(entry, "last");
    avatar.mPassword   = mapString(entry, "password");
    avatar.mLoginUri   = mapString(entry, "login_uri");
    avatar.mGrid       = mapString(entry, "grid");
    avatar.mMfaCommand = mapString(entry, "mfa_command");
    if (entry.has("mfa_window_guard_secs"))
    {
        avatar.mMfaWindowGuardSecs = entry["mfa_window_guard_secs"].asInteger();
    }

    if (avatar.mFirst.empty() || avatar.mLast.empty() || avatar.mPassword.empty())
    {
        error = path + ": avatar '" + key
              + "' needs first, last and password";
        return false;
    }

    return true;
}

bool loadGrid(const std::string& path, Grid& grid, std::string& error)
{
    LLSD doc;
    if (!parseTomlFile(path, doc, error))
    {
        return false;
    }

    grid = Grid();
    grid.mLoginUri  = mapString(doc, "login_uri");
    grid.mGridName  = mapString(doc, "gridname");
    grid.mGridNick  = mapString(doc, "gridnick");
    grid.mHelperUri = mapString(doc, "helperuri");
    grid.mLoginPage = mapString(doc, "loginpage");
    grid.mSlurlBase = mapString(doc, "slurl_base");
    grid.mPlatform  = mapString(doc, "platform");

    if (grid.mLoginUri.empty())
    {
        error = path + ": missing required 'login_uri'";
        return false;
    }
    return true;
}

std::string gridNameFromLoginUri(const std::string& login_uri)
{
    std::string s = login_uri;

    const size_t scheme = s.find("://");
    if (scheme != std::string::npos)
    {
        s.erase(0, scheme + 3);
    }
    // Drop any path, query or fragment; keep host[:port].
    const size_t cut = s.find_first_of("/?#");
    if (cut != std::string::npos)
    {
        s.erase(cut);
    }
    LLStringUtil::toLower(s);
    return s;
}

} // namespace FSTestConfig
