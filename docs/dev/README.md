<!-- SPDX-License-Identifier: MIT -->
<!-- Copyright (c) 2026 The RadioFoundry Authors -->

# Contributor guide

These pages cover how to build, test, release, and extend RadioFoundry. They describe process and tooling, and link
to the [specifications](../spec/README.md) for behavior. For the full list of pages, see the
[documentation map](../README.md).

## Start

| To | Read |
|---|---|
| Learn the rules every change follows | [Contributing](contributing.md), [Clean-room rules](../CLEANROOM.md) |
| Find your way around the code | [Architecture](architecture.md) |
| Write or edit documentation | [Documentation style](doc-style.md) |

## Build and test

| To | Read |
|---|---|
| Build a radio's image and flash it | [Building](building.md) |
| Turn a feature on or off, or add a feature switch | [Build configuration](../build-config.md) |
| Add a host test or a HIL test | [Testing](testing.md) |
| Run everything a pull request must pass (`JOBS=8 tools/ci.sh`) | [Continuous integration](ci.md) |
| Set up a CI runner or a toolchain image | [CI runners](ci-runners.md) |
| Check a task's stack depth | [Stack check](stack-check.md) |
| Drive a radio from a script | [Debug link tool](debug-link.md) |
| Change the UI | [User interface spec](../spec/ui.md), then regenerate the gallery as [Testing](testing.md) describes |

## Add to the firmware

| To | Read |
|---|---|
| Port RadioFoundry to a new radio | [Adding a radio](adding-a-radio.md), [Board registry](board-registry.md) |
| Add a digital mode | [Adding a mode](adding-a-mode.md) |
| Change how the codec reaches a radio | [The AMBE codec](codec.md) |
| Translate the radio or the site | [Translating](translating.md) |

## Ship

| To | Read |
|---|---|
| Cut a release | [Releases](release.md) |
| Build the user manual PDF | [Manual PDF](manual-pdf.md) |
| Change the web site | [Web site](website.md) |
| Change the browser installer | [Web flasher](web-flasher.md) |
| Change the browser codeplug editor or the CPS shim | [Codeplug editor](codeplug-editor.md) |

Terms used in the code and specs are in the [developer glossary](glossary.md).
