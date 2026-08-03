// Profile validation (spec section 10.9, 11.6; selection spec 18).
#pragma once

#include <string>
#include <vector>

#include "Profile.h"

namespace gmb {

struct ValidationIssue {
    enum class Severity : uint8_t { Error, Warning };
    Severity severity = Severity::Error;
    std::string field;
    std::string message;
};

class ProfileValidator {
public:
    // Returns all issues. The profile is safe to activate only when there are no
    // Error-severity issues (spec section 10.9).
    static std::vector<ValidationIssue> validate(const Profile& p);

    static bool isActivatable(const Profile& p) {
        for (const auto& i : validate(p))
            if (i.severity == ValidationIssue::Severity::Error) return false;
        return true;
    }
};

}  // namespace gmb
