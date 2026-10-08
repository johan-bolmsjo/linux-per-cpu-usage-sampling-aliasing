# Linux per-CPU Usage Sampling Aliasing Demonstration

## Rules of Engagement

### Naming rules

- Never abbreviate named (not anonymous) types or functions.

### Scoping rules

- Use the minimum scope possible for definitions.

### Documentation style

- Use plain language without contrived metaphors.
- Never use abbreviations except explicitly accepted ones.
- Use Go's comment style for types: Lead with the name of the function or type it applies to.
- Comment what is, not what was!
- Never comment historical details or circumstances.
- Never refer to external documents or task instructions in comments.

#### Accepted abbreviations

- e.g. (exempli gratia: for example)
- i.e. (id est: that is)
- API (application programming interface)

### Design methodologies

- Use consistent naming of functions, types and variables.
- Apply separation of concerns: Consider the responsibilities of code modules with semantically clear interfaces.
- Keep code "DRY": Avoid excessive copy paste.
- Avoid inline magic constants: Name constants to provide context.

### Unit testing

- Group tests by tested functionality.
- Keep tests focused to a single property.
- Use table driven tests for multiple inputs.
- Minimize the number of tests needed for test coverage.
- Comment each test function, explaining its purpose.
- Make sure that unit tests fail without the associated correction.
