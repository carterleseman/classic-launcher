#pragma once

#include <string>
#include <vector>

// ---------------------------------------------------------------------------
//  Saved login accounts stored at %APPDATA%\wizlauncher\accounts.json.
//  Passwords are DPAPI-encrypted, base64-encoded blobs.
// ---------------------------------------------------------------------------

struct Account {
    std::string username;
    std::string encrypted_password;
};

std::string accounts_path();

std::vector<Account> accounts_load();
void accounts_save(const std::vector<Account>& accounts);

enum class AccountSaveResult { Failed, Added, Updated };

// Add a new account or overwrite the password for an existing username.
AccountSaveResult accounts_add_or_update(const std::string& username,
                                         const std::string& plaintext_password);

void accounts_remove(const std::string& username);

// Decrypt and return the password for username.
std::string accounts_get_password(const std::string& username);
