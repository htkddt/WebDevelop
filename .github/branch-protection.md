# Required repository rule for `main` / `master`

GitHub Actions can validate code, but a workflow YAML file cannot by itself reject a direct git push. Configure a GitHub ruleset (preferred) or classic branch protection rule in **Settings → Rules** for `main` and `master`:

1. Require a pull request before merging; require at least one approval.
2. Require status checks to pass, and select `frontend-build`, `nest-build`, and `ai-test` from the CI workflow.
3. Require the branch to be up to date before merging and require all review conversations to be resolved.
4. Block force pushes and deletions; do not allow bypassing rules, including for administrators unless there is an explicit emergency policy.
5. Restrict direct pushes to the protected branches.

The `CI` workflow runs for pull requests into protected branches and for every push to a non-protected branch. `Production deployment` only runs after the PR merge creates a push to `main` or `master`.
