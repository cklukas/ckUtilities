// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: GPL-3.0-or-later
#include "child_lookup.hpp"
#include <stdexcept>
#include <cerrno>

namespace ck::files {
ChildLookup::ChildLookup(ChildLookupContext context, Payload payload)
    : context_(std::move(context)), payload_(std::move(payload)) {
    if (!context_->requested.is_valid()||!valid_child_lookup_mode(context_->mode))
        throw std::invalid_argument("invalid child lookup context");
}
ChildLookup::ChildLookup(ChildLookup&& other) noexcept
    : context_(std::move(other.context_)), payload_(std::move(other.payload_)) {
    other.context_.reset();other.payload_.emplace<0>();
}
ChildLookup& ChildLookup::operator=(ChildLookup&& other) noexcept {
    if(this!=&other){context_=std::move(other.context_);payload_=std::move(other.payload_);other.context_.reset();other.payload_.emplace<0>();}
    return *this;
}
ChildLookup ChildLookup::found(ChildLookupContext context, std::unique_ptr<OpenedEntry> entry) {
    if(!entry)throw std::invalid_argument("found lookup needs an owning entry");
    return ChildLookup(std::move(context),Payload(std::in_place_index<1>,std::move(entry)));
}
ChildLookup ChildLookup::missing(ChildLookupContext context, NativeError witness) {
    const bool domain=witness.domain==ErrorDomain::Posix||witness.domain==ErrorDomain::Win32||
        witness.domain==ErrorDomain::NtStatus||witness.domain==ErrorDomain::Adapter;
    if(!domain||witness.code==0||witness.operation.empty())
        throw std::invalid_argument("missing lookup needs explicit evidence");
    return ChildLookup(std::move(context),Payload(std::in_place_index<2>,std::move(witness)));
}
ChildLookupState ChildLookup::state() const noexcept {
    if(payload_.index()==1)return ChildLookupState::Found;
    return payload_.index()==2?ChildLookupState::Missing:ChildLookupState::Unknown;
}
const OpenedEntry* ChildLookup::entry() const noexcept {
    const auto* owned=std::get_if<1>(&payload_);return owned?owned->get():nullptr;
}
const NativeError* ChildLookup::missing_witness() const noexcept {return std::get_if<2>(&payload_);}
namespace detail {
bool definite_child_absence(const UnavailableObservation& error, ChildLookupFamily family) noexcept {
    if(error.state!=ObservationState::Gone)return false;
    switch(family) {
    case ChildLookupFamily::PosixAt:return error.error.domain==ErrorDomain::Posix&&error.error.code==ENOENT;
    case ChildLookupFamily::WindowsNt:return error.error.domain==ErrorDomain::NtStatus&&
        (error.error.code==0xc0000034LL||error.error.code==0xc000000fLL);
    }
    return false;
}
}
} // namespace ck::files
