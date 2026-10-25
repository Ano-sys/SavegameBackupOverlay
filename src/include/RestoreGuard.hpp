#pragma once

#include "BackupTypes.hpp"

#include <algorithm>
#include <vector>

namespace backup {

enum class RestoreChoice { Check,
    BackupFirst,
    WithoutBackup };
enum class RestoreAction { Ask,
    Upload,
    Restore };

inline RestoreAction restore_action(const std::string& current_hash, const std::vector<CloudBackup>& backups, RestoreChoice choice, const std::string& confirmed_hash) {
    if(current_hash.empty()) return RestoreAction::Ask;
    if(std::any_of(backups.begin(), backups.end(), [&](const CloudBackup& entry) { return entry.save_hash == current_hash; })) return RestoreAction::Restore;
    if(choice == RestoreChoice::Check || current_hash != confirmed_hash) return RestoreAction::Ask;

    return choice == RestoreChoice::BackupFirst ? RestoreAction::Upload : RestoreAction::Restore;
}

} // namespace backup
