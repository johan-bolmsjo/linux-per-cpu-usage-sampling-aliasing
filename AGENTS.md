# Linux per-CPU Usage Sampling Aliasing Demonstration

## Rules of Engagement

### Naming rules

- Never abbreviate named (not anonymous) types or functions.

### Scoping rules

- Use the minimum scope for definitions based on their use.

### Comment style

- Never use abbreviations except exempted ones.
- Use Go's comment style for types, which leads with the name of the function or type it applies to.
- Document the current code behavior.
- Use self contained comments: never refer to external documents or task instructions in comments.
- Never mention historical details or circumstances.

#### Exempt abbreviations in comments

- e.g. (exempli gratia: for example)
- i.e. (id est: that is)
- API (application programming interface)

### Design methodologies

- Use consistent naming of functions, types and variables.
- Apply separation of concerns: Consider the responsibilities of code modules for clear interfaces.
- Keep code "DRY": Avoid excessive copy paste.
- Avoid inline magic constants: give constant names to provide context.

### Unit testing

- Group tests by tested functionality.
- Keep tests focused to a single property.
- Use table driven tests for multiple inputs.
- Minimize the number of tests needed for test coverage.
- Comment each test function, explaining its purpose.
- Make sure that unit tests fail without the associated correction.
