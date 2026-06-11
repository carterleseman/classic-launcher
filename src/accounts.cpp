#include "accounts.h"
#include "crypto.h"
#include "json.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

std::string accounts_path()
{
    char appdata[MAX_PATH]{};
    if (!GetEnvironmentVariableA("APPDATA", appdata, sizeof(appdata)))
        return {};
    return std::string(appdata) + "\\wizlauncher\\accounts.json";
}

std::vector<Account> accounts_load()
{
    std::vector<Account> result;
    std::string json = json_read_text_file(accounts_path());
    if (json.empty()) return result;

    for (const auto& entry : json_parse_accounts(json)) {
        result.push_back({ entry.username, entry.password });
    }
    return result;
}

void accounts_save(const std::vector<Account>& accounts)
{
    std::vector<JsonAccountEntry> entries;
    entries.reserve(accounts.size());
    for (const auto& acct : accounts)
        entries.push_back({ acct.username, acct.encrypted_password });

    json_write_text_file(accounts_path(), json_serialize_accounts(entries));
}

AccountSaveResult accounts_add_or_update(const std::string& username, const std::string& plaintext_password)
{
    if (username.empty()) return AccountSaveResult::Failed;

    auto accounts = accounts_load();
    std::string encrypted = crypto_dpapi_encrypt(plaintext_password);
    if (encrypted.empty()) return AccountSaveResult::Failed;

    for (auto& acct : accounts) {
        if (acct.username == username) {
            acct.encrypted_password = encrypted;
            accounts_save(accounts);
            return AccountSaveResult::Updated;
        }
    }

    accounts.push_back({ username, encrypted });
    accounts_save(accounts);
    return AccountSaveResult::Added;
}

void accounts_remove(const std::string& username)
{
    if (username.empty()) return;

    auto accounts = accounts_load();
    std::vector<Account> kept;
    kept.reserve(accounts.size());
    for (auto& acct : accounts) {
        if (acct.username != username)
            kept.push_back(std::move(acct));
    }
    accounts_save(kept);
}

std::string accounts_get_password(const std::string& username)
{
    if (username.empty()) return {};

    for (const auto& acct : accounts_load()) {
        if (acct.username == username)
            return crypto_dpapi_decrypt(acct.encrypted_password);
    }
    return {};
}
