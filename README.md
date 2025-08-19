# Espruino_xfsm
xstate inspired FSM Library for Espruino JavaScript Interpreter

This Espruino Library provides an **XState v4 FSM–equivalent** finite state machine (FSM)engine implemented in **C**, exposed to Espruino JavaScript via the standard library mechanism (jswrap). The goal is **full feature parity with the XState v4 `xstate-fsm` package** (minimal ~1 kB FSM), while aligning with XState semantics to enable tooling, visualization, and future AI-assisted workflows. A future roadmap includes **hierarchical (nested) states** and deeper **SCXML alignment**.

## Credits and Acknowledgements

### Espruino
This project builds on the **[Espruino JavaScript Interpreter](https://www.espruino.com/)**, an open-source JavaScript engine designed for microcontrollers.  
Espruino is developed and maintained by **Gordon Williams** and contributors.  

**Copyright © 2013–2025 Gordon Williams**  
Licensed under the **[Mozilla Public License 2.0](https://www.mozilla.org/en-US/MPL/2.0/)**.  

We gratefully acknowledge the Espruino project for providing the foundation and ecosystem into which this FSM library is integrated.

### XState
This project draws conceptual inspiration and API alignment from **[XState](https://stately.ai/docs/xstate/)**, a state machine and statecharts library for JavaScript and TypeScript.  
Specifically, the initial requirements are derived from the **[XState v4 FSM package](https://stately.ai/docs/xstate-v4/xstate/packages/xstate-fsm)**, which provided a minimal implementation of finite state machines in JavaScript.

**Copyright © 2017–2025 Stately.ai and David Khourshid**  
Licensed under the **[MIT License](https://opensource.org/licenses/MIT)**.  

We acknowledge the XState project as the primary inspiration for the semantics, syntax, and long-term vision of this FSM library, and recognize the value of its ecosystem in advancing state machine and statechart adoption.

---

### Standards
This work also references the **[W3C SCXML: State Chart XML (State Machine Notation for Control Abstraction)](https://www.w3.org/TR/scxml/)** specification.  
We acknowledge the W3C standards community for providing a formal, machine-readable basis for state machine semantics.

---

### Project Note
This FSM library is an independent work intended to integrate XState-like semantics into the Espruino ecosystem. It is **not affiliated with or endorsed by Espruino, Stately.ai, or the W3C**.  
All copyrights remain with their respective owners.
