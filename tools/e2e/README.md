# Live agent scenarios

`agents.py` drives the Agent Library through a deployed Lake's public API. T1–T10 cover reads,
proposals, delegation, agent tasks, and workspace pub/sub. T11 starts ApplicationAgent, validates
its proposal, sends the exact object back as `context.confirmed_proposal`, waits for deployment,
and requests the resulting App URL. T12 asks ExperimentsAgent for a study, confirms the proposal,
waits for trials and requests the explanation.

The suite changes the live customer only in scenarios whose descriptions say so. Pass the base
URL, a file containing the person's Lake token, the customer id, and optional scenario names. Do
not run it as part of the native CTest proof.
