# Skr8tr — Sovereign Web Microservices Platform

**The orchestrator built for web developers.**

Deploy microservices fast. Stay compliant. Sleep at night.

No Docker. No YAML. No Kubernetes. 5 MB control plane. Post-quantum auth on every command. Runs on Google Cloud Platform.

---

## See It Live

**[demo.skr8tr.online](https://demo.skr8tr.online)** — A full e-commerce platform: 9 Rust microservices, Angular SPA, HTTP/2 ingress with Let's Encrypt TLS, running on 3 GCP VMs. No Kubernetes. No containers. Orchestrated entirely by Skr8tr.

---

## What Skr8tr Is

Skr8tr is a proprietary sovereign workload orchestrator purpose-built for web teams who have outgrown a single server but don't want to become Kubernetes engineers.

Three binaries run your entire control plane. Any GCP VM. Any scale.

| | Kubernetes (GKE) | Skr8tr on GCP |
|---|---|---|
| Control plane size | 600+ MB | 5 MB |
| Config format | YAML + CRDs + Helm | 6-line `.skr8tr` manifest |
| Container runtime | Docker / containerd required | None — fork + exec |
| HTTP ingress | Separate Helm chart | Built in (HTTP/2 + TLS) |
| Auth | Bearer tokens (base64) | ML-DSA-65 post-quantum signatures |
| Audit trail | Falco + SIEM plumbing | SHA-256 chain, built in |
| Deploy time | 30–120s (image pull) | Under 2 seconds |

---

## Platform Features

**Orchestration core:**
- Masterless mesh — no etcd, no leader election, no SPOF
- Rolling updates, health checks, restart policies, graceful drain
- Service discovery via built-in Tower registry
- cgroups v2 resource limits per workload
- Prometheus metrics on every node
- Secret injection, persistent volumes, remote exec

**Enterprise layer (all included):**
- HTTP/2 ingress with TLS termination and ALPN negotiation
- RBAC with per-team ML-DSA-65 signing keys and namespace isolation
- Cryptographic audit chain — SHA-256 chained, AES-256-GCM at rest
- RFC 5424 syslog forwarding to any SIEM
- SSO / OIDC identity bridge (Okta, Azure AD, Google Workspace)
- Multi-tenant conductor with per-namespace resource quotas
- CPU / RAM autoscaler

---

## Website

Full documentation, architecture details, and pricing at **[skr8tr.online](https://skr8tr.online)**.

---

## Contact

**[scottbaker@rusticagentic.agency](mailto:scottbaker@rusticagentic.agency)**

All serious inquiries answered within 24 hours.

---

*© 2026 Skr8tr. Proprietary software. All rights reserved.*
