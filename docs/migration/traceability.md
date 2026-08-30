# ckVision product traceability

This ledger connects each native product to its core evidence, presentation
scenario, and release gate.

| Product | Domain evidence | Native scenario | Release evidence |
| --- | --- | --- | --- |
| `ck-utilities` | [tool catalogue](../../tests/unit/app_info/app_info_tests.cpp) | [launcher](../../src/vision/launcher/tests/launcher_app_tests.cpp) | [product tree](../../cmake/VerifyCkVisionProductTree.cmake) |
| `ck-json-view` | [JSON core](../../tests/unit/json_view/json_view_core_tests.cpp) | [JSON View](../../src/vision/json-view/tests/json_view_app_tests.cpp) | [installed product](../../cmake/VerifyCkVisionInstall.cmake) |
| `ck-find` | [search core](../../tests/unit/ck_find/search_backend_tests.cpp) | [Find](../../src/vision/find/tests/find_app_tests.cpp) | [installed archive](../../cmake/VerifyCkVisionArchive.cmake) |
| `ck-du` | [disk usage](../../tests/unit/ck_du/disk_usage_tests.cpp) | [Disk Usage](../../src/vision/du/tests/disk_usage_app_tests.cpp) | [terminal profile](../../cmake/VerifyCkVisionTerminal.cmake) |
| `ck-config` | [option registry](../../tests/unit/config/option_registry_tests.cpp) | [Config](../../src/vision/config/tests/config_app_tests.cpp) | [SDK consumer](../../cmake/VerifyCkVisionConsumer.cmake) |
| `ck-edit` | [Markdown](../../tests/unit/ck_edit/markdown_transformations_tests.cpp) | [Edit](../../src/vision/edit/tests/edit_app_tests.cpp) | [package rehearsal](../../cmake/CkVisionPackage.cmake) |
| `ck-chat` | [AI core](../../tests/unit/ckai_core/llm_tests.cpp) | [Chat](../../src/vision/chat/tests/chat_app_tests.cpp) | [opt-in model hook](ckvision-baseline.md) |

The release gate requires an installed SDK consumer, staged product, extracted
archive, and deterministic CTest suite. The source-level ckVision-only guard
and the installed-artifact verifier prevent non-native UI dependencies from
returning to the product branch.

Framework improvements require a separate problem statement, public contract,
focused framework coverage, and one adopting-product scenario before a native
package may depend on them. See [the roadmap](../../tv_to_ckvision.md).
