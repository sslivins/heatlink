/// @file token_store.h
/// @brief Integration tokens (Authorization: Bearer hlt1_...) persisted in NVS.
///
/// Thin ESP-IDF wrapper around components/tokens: random generation, SHA-256
/// hashing and NVS storage. Thread-safe.
#pragma once

#include <string>
#include <vector>

#include "token_logic.h"

/// Load tokens from NVS. Idempotent; needs nvs_flash_init().
void token_store_init();

/// Create a token. On success @p token_out holds the full token text (shown
/// once) and @p entry_out its stored record. False if the label is empty after
/// sanitizing, the table is full, or persisting failed.
bool token_store_create(const std::string& label, tokens::Scope scope, std::string* token_out,
                        tokens::Entry* entry_out);

/// Revoke by id; false if unknown.
bool token_store_revoke(const std::string& id);

/// Revoke every token (admin-password reset).
void token_store_revoke_all();

/// Snapshot of the stored records (hashes included; don't expose them).
std::vector<tokens::Entry> token_store_list();

/// Validate a token's text. On success fills @p out and records the use.
bool token_store_validate(const std::string& token, tokens::Entry* out);
