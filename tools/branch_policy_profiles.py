#!/usr/bin/env python3
"""Named branch rankwidth-delegation policy profiles for experiments."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class BranchPolicyProfile:
    name: str
    description: str
    args: tuple[str, ...]


PROFILES: tuple[BranchPolicyProfile, ...] = (
    BranchPolicyProfile(
        "current-native",
        "Current branch path: native min-fill-cut rankwidth source with built-in thresholds.",
        ("--branch-rw-source", "native"),
    ),
    BranchPolicyProfile(
        "no-rankwidth",
        "Disable rankwidth handoff; treewidth handoff and pure branch remain active.",
        ("--branch-rw-source", "none"),
    ),
    BranchPolicyProfile(
        "from-treewidth-default",
        "Use the optimized from-treewidth rank decomposition with built-in thresholds.",
        ("--branch-rw-source", "from-treewidth"),
    ),
    BranchPolicyProfile(
        "both-default",
        "Probe native and from-treewidth rank decompositions and keep the better forecast.",
        ("--branch-rw-source", "both"),
    ),
    BranchPolicyProfile(
        "both-permissive",
        "More aggressively try rankwidth when forecasts are close.",
        (
            "--branch-rw-source", "both",
            "--branch-rw-min-treewidth-width", "2",
            "--branch-rw-min-treewidth-forecast", "512",
            "--branch-rw-min-residual-vars", "16",
            "--branch-rw-low-rank-bypass", "4",
            "--branch-rw-min-speedup", "1.1",
            "--branch-rw-fixed-overhead-ns", "20000",
            "--branch-tw-fixed-overhead-ns", "10000",
        ),
    ),
    BranchPolicyProfile(
        "from-treewidth-permissive",
        "Aggressively try only the low-overhead from-treewidth rank decomposition.",
        (
            "--branch-rw-source", "from-treewidth",
            "--branch-rw-min-treewidth-width", "2",
            "--branch-rw-min-treewidth-forecast", "512",
            "--branch-rw-min-residual-vars", "16",
            "--branch-rw-low-rank-bypass", "4",
            "--branch-rw-min-speedup", "1.1",
            "--branch-rw-fixed-overhead-ns", "20000",
            "--branch-tw-fixed-overhead-ns", "10000",
        ),
    ),
    BranchPolicyProfile(
        "both-conservative",
        "Only use rankwidth when treewidth pressure is clearly high and the model predicts a win.",
        (
            "--branch-rw-source", "both",
            "--branch-rw-min-treewidth-width", "6",
            "--branch-rw-min-treewidth-forecast", "8192",
            "--branch-rw-min-residual-vars", "48",
            "--branch-rw-low-rank-bypass", "3",
            "--branch-rw-min-speedup", "1.8",
            "--branch-rw-fixed-overhead-ns", "100000",
            "--branch-tw-fixed-overhead-ns", "10000",
        ),
    ),
)


def profile_by_name(name: str) -> BranchPolicyProfile:
    for profile in PROFILES:
        if profile.name == name:
            return profile
    known = ", ".join(profile.name for profile in PROFILES)
    raise KeyError(f"unknown branch policy profile {name!r}; known: {known}")


def profile_args(name: str) -> list[str]:
    return list(profile_by_name(name).args)


def profile_dict(profile: BranchPolicyProfile) -> dict[str, object]:
    return {
        "name": profile.name,
        "description": profile.description,
        "args": list(profile.args),
    }
