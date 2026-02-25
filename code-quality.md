# code quality

1. correctness - code that does what we want
2. maintainability - it's easy to add new features and easier to fix defects (bugs)

The purpose of code quality is to achieve correctness and maintainability. Code review supports code quality. A review is constructive.  A review is composed of comments and suggestions.

# concepts

code is a liability
less is more
simple is best
correct by design

## worst

complexity - the greatest enemy!
coupling - source of complexity!
wrong abstraction - expensive forever!

## bad

relying on side effects
global state
unnecessary layers of abstraction
side effects
mutable objects
huge comments - variables and class names should explain why
code must be kept in sync

## good

strong typing catches issues at compile time
unit tests prevent regressions and toil
directory structure matches layers and components
proper layering
minimal code
separation of concerns
encapsulation over inheritance
leverage runtime behavior - polymorphism branches
leverage language features - reduces code
classes have one purpose (SRP)
functional procedures
enforced valid classes
only one way to do one thing
descriptive data
data driven behavior
descriptive, self-documenting names
immutable objects
consistent naming

# improving codebases

Ask these questions before adding a feature:
Can we get 90% of the requirement with 10% of the code?
Should we refactor before adding behavior to ensure correctness?

Ask these questions after fixing a bug:
What was the **code quality** issue or issues that allowed this bug in the first place?