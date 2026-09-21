# AdvMatch code style

- Follow the style already used by `BasicOrder` and `User`.
- Use `lowerCamelCase` for C++ fields, local variables, parameters, and methods. Use `UpperCamelCase` for types and enum members. Do not add leading or trailing underscores to project-defined identifiers.
- Qualify member access in method bodies with `this->` when it improves clarity, as in `BasicOrder` and `User`.
- Use four-space indentation and put opening braces on the next line for classes, namespaces, functions, and control statements.
- Always use braces for `if`, `else`, and loops. Separate logical sections with a blank line. Keep expressions on one line when they fit naturally, and break them only when needed for readability.
- Keep required external spellings unchanged: C ABI exports such as `advmatch_abi_version`, JNI entry points such as `JNI_OnLoad`, library names, standard-library names, and third-party APIs.
