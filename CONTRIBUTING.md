# Contributing

Corrections, questions and reproductions on other boards are welcome. Open an issue first for anything larger than a fix, so the approach can be agreed before you spend time on it.

## What makes a change acceptable

1. **Claims come with evidence.** A new number needs a record under `evidence/` and a manifest entry saying how and under what conditions it was measured. Say which kind it is: simulation, tool output or board measurement.
2. **The baseline stays green.** RTL changes must pass `make -C hw sim`. Changes that affect the fabric, the firmware or the image must pass the board suite (`tests/board/pfdaq_final_suite.sh`, 27 of 27) before they reach `main`. If you cannot run the board suite, say so in the pull request.
3. **Experiments are labelled.** New work that has not been through the board suite carries `status: experimental` in its page and lives on a branch or under `experiments/` until it has.
4. **Nothing invented.** Addresses, register bits, pin names and tool options come from the source or from vendor documents you can cite.
5. **No vendor material.** Do not add Microchip IP sources, netlists, programming files, tool binaries or copies of vendor documents.

## Checks to run

```sh
make -C hw sim                    # both testbenches, each judged by its Python model
python3 tools/gen_docs.py         # refresh generated indexes and navigation
python3 tools/check_content.py    # references, source symbols, evidence, links
```

## Writing documentation

Read [docs/writing-guide.md](docs/writing-guide.md). Pages are Markdown with front matter; the catalog, learning path, system pages and navigation are generated from it.

## Commit messages

One line saying what changed, a blank line, then why and how it was verified. Mention the evidence class: "simulation", "build", "board".

## Safety

This project never reprograms the board's boot flash (eNVM). Programming jobs contain the FABRIC and SNVM components only. Changes that would alter that need a very good reason and a recovery plan in the pull request.
