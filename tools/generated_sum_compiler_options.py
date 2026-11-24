"""Validate the closed compiler options for generated Sum v2 manifests."""

import shlex

OPTIONS = {"preset-default": [], "align32": ["-falign-loops=32"]}

SCALAR_DISABLES = {
    "GNU": {"-fno-tree-loop-vectorize", "-fno-tree-slp-vectorize"},
    "Clang": {"-fno-vectorize", "-fno-slp-vectorize"},
}

SCALAR_CONFLICTS = {
    "-flto", "-funroll-loops", "-funroll-all-loops", "-floop-unroll-and-jam",
    "-fvectorize", "-fslp-vectorize", "-fslp-vectorize-aggressive",
    "-ftree-vectorize", "-ftree-loop-vectorize", "-ftree-slp-vectorize",
    "-vectorize-loops", "-vectorize-slp",
}


def compiler_option_facts(option: str, command_row: dict, compiler_id: str) -> dict:
    command = command_row.get("command")
    arguments = command_row.get("arguments")
    if command is not None:
        if not isinstance(command, str) or not command.strip():
            raise ValueError("Invalid actual compile command")
        tokens = shlex.split(command)
        if arguments is not None and arguments != tokens:
            raise ValueError("Actual compile command and argument list disagree")
        arguments = tokens
    if not isinstance(arguments, list) or not arguments or not all(isinstance(arg, str) for arg in arguments):
        raise ValueError("Missing actual compile command or argument list")
    if compiler_id not in SCALAR_DISABLES or option not in OPTIONS:
        raise ValueError("Unsupported compiler or generated Sum compiler option")

    required = SCALAR_DISABLES[compiler_id] | {"-fno-lto", "-fno-unroll-loops"}
    missing = required.difference(arguments)
    conflicts = {arg for arg in arguments if arg.partition("=")[0] in SCALAR_CONFLICTS}
    if missing or conflicts:
        raise ValueError("Actual compile command violates fixed scalar controls "
                         f"(missing {sorted(missing)}, conflicting {sorted(conflicts)})")

    alignment = [arg for arg in arguments if arg.partition("=")[0] in ("-falign-loops", "-fno-align-loops")]
    if alignment != OPTIONS[option]:
        raise ValueError("Actual compile command does not match the closed scalar compiler option")
    return {"id": option, "flags": alignment, "candidate_population": list(OPTIONS)}
