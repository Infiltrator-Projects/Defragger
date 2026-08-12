# SPDX-License-Identifier: GPL-3.0-or-later
"""Pure presentation policy for operation lifecycle status."""

from __future__ import annotations

from dataclasses import dataclass

from core.protocol import OperationResult


def operation_display_name(purpose: str) -> str:
    return {
        "analysis": "Analysis",
        "defrag": "Defragment",
        "growth-defrag": "Growth Defrag",
        "recover": "Recovery",
    }.get(purpose, purpose.replace("-", " ").title())


@dataclass(frozen=True, slots=True)
class CompletionPresentation:
    progress_text: str
    status_text: str
    post_analysis_status: str | None = None
    post_analysis_progress_text: str | None = None


def successful_completion(
    purpose: str,
    result: OperationResult | None,
) -> CompletionPresentation:
    """Translate a typed worker result into GUI text."""

    display_name = operation_display_name(purpose)
    if (
        result is not None
        and result.operation == purpose
        and result.status == "not-needed"
    ):
        if purpose == "growth-defrag":
            return CompletionPresentation(
                progress_text="Not needed",
                status_text=(
                    "Growth Defrag not needed; the existing layout already "
                    "satisfies the 10% reserve."
                ),
                post_analysis_status=(
                    "Growth Defrag not needed · existing 10% growth-space layout verified"
                ),
                post_analysis_progress_text="Not needed",
            )
        return CompletionPresentation(
            progress_text="Not needed",
            status_text=f"{display_name} not needed; the canonical layout is already verified.",
            post_analysis_status=(
                f"{display_name} not needed · canonical filesystem layout verified"
            ),
            post_analysis_progress_text="Not needed",
        )
    return CompletionPresentation(
        progress_text="Complete",
        status_text=f"{display_name} completed successfully.",
    )
