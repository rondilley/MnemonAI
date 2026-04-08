"""
LongMemEval Synthetic Dataset for mnemond

Generates benchmark questions across the 5 LongMemEval memory ability categories:
  1. Information Extraction (IE)  - Recall specific facts from stored sessions
  2. Multi-Session Reasoning (MR) - Synthesize information across multiple sessions
  3. Knowledge Updates (KU)       - Track evolving user information over time
  4. Temporal Reasoning (TR)      - Answer questions requiring time awareness
  5. Abstention (ABS)             - Correctly refuse when info is not available

Each question is a dict with:
  - question_id:    Unique identifier
  - category:       One of IE, MR, KU, TR, ABS
  - sessions:       List of (timestamp, content) tuples to ingest as memories
  - query:          Search query to issue against mnemond
  - query_type:     "keyword", "hybrid", or "temporal"
  - expected:       List of substrings that MUST appear in retrieved results
  - negative:       List of substrings that must NOT appear (for KU/ABS)
  - temporal_args:  Extra args for temporal queries (since/until)
  - description:    Human-readable description of what's being tested

Reference: LongMemEval (Wu et al., ICLR 2025) - https://arxiv.org/abs/2410.10813
"""


def generate_dataset():
    """Return list of benchmark questions."""
    questions = []

    # =========================================================================
    # Category 1: Information Extraction (IE)
    # Ability to recall specific details from stored memories
    # =========================================================================

    questions.append({
        "question_id": "IE-001",
        "category": "IE",
        "description": "Single-session fact recall: user's programming language",
        "sessions": [
            ("2025-06-15T10:00:00Z",
             "The user mentioned they primarily work with Rust and have been "
             "using it for three years. They particularly enjoy the borrow "
             "checker and pattern matching features."),
        ],
        "query": "programming language user works with",
        "query_type": "keyword",
        "expected": ["Rust"],
        "negative": [],
    })

    questions.append({
        "question_id": "IE-002",
        "category": "IE",
        "description": "Single-session fact recall: specific numeric detail",
        "sessions": [
            ("2025-07-01T14:30:00Z",
             "The team decided to set the maximum batch size to 512 items "
             "for the data pipeline. This was chosen because larger batches "
             "caused out-of-memory errors on the staging server which has "
             "only 16GB RAM."),
        ],
        "query": "batch size data pipeline",
        "query_type": "keyword",
        "expected": ["512"],
        "negative": [],
    })

    questions.append({
        "question_id": "IE-003",
        "category": "IE",
        "description": "Single-session fact recall: assistant-provided info",
        "sessions": [
            ("2025-06-20T09:00:00Z",
             "The PostgreSQL connection pool was configured with a minimum of "
             "5 connections and a maximum of 25 connections. The idle timeout "
             "was set to 300 seconds based on the p99 query latency analysis."),
        ],
        "query": "PostgreSQL connection pool configuration",
        "query_type": "keyword",
        "expected": ["25", "300"],
        "negative": [],
    })

    questions.append({
        "question_id": "IE-004",
        "category": "IE",
        "description": "Recall specific tool/library recommendation",
        "sessions": [
            ("2025-08-10T11:00:00Z",
             "For the new monitoring stack, the team chose Prometheus for "
             "metrics collection, Grafana for dashboards, and Loki for log "
             "aggregation. AlertManager handles notification routing to the "
             "on-call PagerDuty channel."),
        ],
        "query": "monitoring stack tools chosen",
        "query_type": "keyword",
        "expected": ["Prometheus", "Grafana", "Loki"],
        "negative": [],
    })

    questions.append({
        "question_id": "IE-005",
        "category": "IE",
        "description": "Recall nested technical detail",
        "sessions": [
            ("2025-09-01T16:00:00Z",
             "The authentication system uses JWT tokens with RS256 signing. "
             "Access tokens expire after 15 minutes and refresh tokens after "
             "7 days. The JWKS endpoint is cached for 1 hour to reduce load "
             "on the identity provider."),
        ],
        "query": "JWT token expiration settings",
        "query_type": "keyword",
        "expected": ["15 minutes", "7 days", "RS256"],
        "negative": [],
    })

    questions.append({
        "question_id": "IE-006",
        "category": "IE",
        "description": "Recall from dense technical session",
        "sessions": [
            ("2025-07-15T08:00:00Z",
             "The Kubernetes cluster runs on three m5.2xlarge control plane "
             "nodes and autoscales worker nodes between 5 and 20 instances "
             "of c5.4xlarge. The cluster uses Calico for CNI networking and "
             "stores secrets in HashiCorp Vault with the Kubernetes auth "
             "backend enabled."),
        ],
        "query": "Kubernetes cluster node configuration",
        "query_type": "keyword",
        "expected": ["m5.2xlarge", "Calico", "Vault"],
        "negative": [],
    })

    questions.append({
        "question_id": "IE-007",
        "category": "IE",
        "description": "Hybrid search for conceptual fact",
        "sessions": [
            ("2025-08-05T13:00:00Z",
             "The decision to use event sourcing instead of traditional CRUD "
             "was driven by the audit requirements from the compliance team. "
             "Every state change must be traceable back to a specific user "
             "action with full before-and-after snapshots."),
        ],
        "query": "why event sourcing was chosen over CRUD",
        "query_type": "hybrid",
        "expected": ["audit", "compliance"],
        "negative": [],
    })

    questions.append({
        "question_id": "IE-008",
        "category": "IE",
        "description": "Recall specific error and resolution",
        "sessions": [
            ("2025-09-12T22:00:00Z",
             "The production outage on September 12th was caused by a DNS "
             "resolution failure in the service mesh. The root cause was an "
             "expired TLS certificate on the internal CA that Istio uses for "
             "mTLS between services. Resolution was to rotate the CA cert "
             "and restart all Envoy sidecars."),
        ],
        "query": "production outage DNS failure",
        "query_type": "keyword",
        "expected": ["Istio", "CA", "certificate"],
        "negative": [],
    })

    # =========================================================================
    # Category 2: Multi-Session Reasoning (MR)
    # Synthesize information across multiple stored memories
    # =========================================================================

    questions.append({
        "question_id": "MR-001",
        "category": "MR",
        "description": "Aggregate: all databases mentioned across sessions",
        "sessions": [
            ("2025-06-01T10:00:00Z",
             "The user data service uses PostgreSQL 15 as its primary "
             "datastore with read replicas in two availability zones."),
            ("2025-06-15T10:00:00Z",
             "The caching layer runs Redis 7 cluster with three shards. "
             "Session data and rate limiting counters live here."),
            ("2025-07-01T10:00:00Z",
             "The search feature was migrated from Elasticsearch to "
             "Meilisearch for better indexing performance and simpler ops."),
            ("2025-07-20T10:00:00Z",
             "Time-series telemetry is stored in InfluxDB with a 90-day "
             "retention policy. Older data is downsampled to 1-hour granularity."),
        ],
        "query": "databases used in the system",
        "query_type": "keyword",
        "expected": ["PostgreSQL", "Redis", "Meilisearch", "InfluxDB"],
        "negative": [],
    })

    questions.append({
        "question_id": "MR-002",
        "category": "MR",
        "description": "Compare: deployment strategies discussed in different sessions",
        "sessions": [
            ("2025-05-01T10:00:00Z",
             "The backend API uses blue-green deployments via AWS CodeDeploy. "
             "Traffic shifts happen over 10 minutes with automatic rollback "
             "if the error rate exceeds 1 percent."),
            ("2025-06-01T10:00:00Z",
             "The frontend SPA uses canary deployments through CloudFront "
             "with Lambda@Edge routing 5 percent of traffic to the new "
             "version first."),
        ],
        "query": "deployment strategy",
        "query_type": "keyword",
        "expected": ["blue-green", "canary"],
        "negative": [],
    })

    questions.append({
        "question_id": "MR-003",
        "category": "MR",
        "description": "Aggregate: team members and their roles",
        "sessions": [
            ("2025-06-10T10:00:00Z",
             "Alice is the tech lead for the payments team and owns the "
             "Stripe integration. She has been at the company for 4 years."),
            ("2025-06-20T10:00:00Z",
             "Bob joined as the new SRE last month. He is responsible for "
             "the Kubernetes infrastructure and on-call rotation."),
            ("2025-07-05T10:00:00Z",
             "Carol is the data engineer who built the ETL pipeline using "
             "Apache Airflow. She also maintains the dbt models."),
        ],
        "query": "team members roles responsibilities",
        "query_type": "hybrid",
        "expected": ["Alice", "Bob", "Carol"],
        "negative": [],
    })

    questions.append({
        "question_id": "MR-004",
        "category": "MR",
        "description": "Cross-session: connect problem to solution",
        "sessions": [
            ("2025-07-01T10:00:00Z",
             "The API gateway is experiencing intermittent 502 errors under "
             "high load. The issue seems related to connection exhaustion "
             "between the gateway and upstream services."),
            ("2025-07-08T10:00:00Z",
             "After investigation, the 502 errors were resolved by increasing "
             "the upstream keepalive connections from 32 to 256 in the nginx "
             "configuration and enabling HTTP/2 to the backend pods."),
        ],
        "query": "502 errors API gateway resolution",
        "query_type": "keyword",
        "expected": ["keepalive", "256", "nginx"],
        "negative": [],
    })

    questions.append({
        "question_id": "MR-005",
        "category": "MR",
        "description": "Aggregate: security measures across sessions",
        "sessions": [
            ("2025-05-15T10:00:00Z",
             "Security: All API endpoints require OAuth 2.0 bearer tokens "
             "issued by the Keycloak identity provider. Scopes are checked "
             "at the API gateway level."),
            ("2025-06-01T10:00:00Z",
             "Security: Database connections use mTLS with certificates "
             "rotated every 30 days via cert-manager. Connection strings "
             "are stored in Vault and injected as environment variables."),
            ("2025-06-20T10:00:00Z",
             "Security: The WAF rules on CloudFront block SQL injection "
             "and XSS patterns. Rate limiting is set to 1000 requests per "
             "minute per IP address."),
        ],
        "query": "security OAuth mTLS WAF",
        "query_type": "keyword",
        "expected": ["OAuth", "Keycloak", "mTLS", "WAF"],
        "negative": [],
    })

    questions.append({
        "question_id": "MR-006",
        "category": "MR",
        "description": "Cross-session: architecture evolution",
        "sessions": [
            ("2025-04-01T10:00:00Z",
             "The notification system was originally a monolithic Django app "
             "that sent emails synchronously during request handling."),
            ("2025-06-01T10:00:00Z",
             "Phase 1 of the notification refactor moved email sending to "
             "a Celery task queue with RabbitMQ as the broker."),
            ("2025-08-01T10:00:00Z",
             "Phase 2 replaced Celery and RabbitMQ with an event-driven "
             "architecture using AWS SNS and SQS. Each notification channel "
             "has its own SQS consumer."),
        ],
        "query": "notification system architecture changes",
        "query_type": "hybrid",
        "expected": ["Django", "Celery", "RabbitMQ", "SNS", "SQS"],
        "negative": [],
    })

    # =========================================================================
    # Category 3: Knowledge Updates (KU)
    # Detect and track evolving information
    # =========================================================================

    questions.append({
        "question_id": "KU-001",
        "category": "KU",
        "description": "Updated preference: programming language change",
        "sessions": [
            ("2025-03-01T10:00:00Z",
             "The user said their preferred editor is VS Code with the "
             "Pylance extension for Python development."),
            ("2025-08-01T10:00:00Z",
             "The user switched to Neovim with LazyVim configuration after "
             "finding VS Code too slow for large monorepos. They now use "
             "telescope.nvim for file navigation."),
        ],
        "query": "preferred code editor",
        "query_type": "keyword",
        "expected": ["Neovim", "LazyVim"],
        "negative": [],
    })

    questions.append({
        "question_id": "KU-002",
        "category": "KU",
        "description": "Updated fact: team size change",
        "sessions": [
            ("2025-01-15T10:00:00Z",
             "The engineering team has 8 members: 5 backend, 2 frontend, "
             "and 1 DevOps engineer."),
            ("2025-04-01T10:00:00Z",
             "After the recent hiring round, the engineering team grew to "
             "12 members. They added 2 more backend engineers, 1 frontend "
             "developer, and 1 dedicated QA engineer."),
        ],
        "query": "engineering team size members",
        "query_type": "keyword",
        "expected": ["12"],
        "negative": [],
    })

    questions.append({
        "question_id": "KU-003",
        "category": "KU",
        "description": "Updated fact: infrastructure migration",
        "sessions": [
            ("2025-02-01T10:00:00Z",
             "All services are deployed on AWS ECS Fargate with CloudFormation "
             "for infrastructure as code."),
            ("2025-07-01T10:00:00Z",
             "The infrastructure migration to Kubernetes on EKS is now "
             "complete. All services have been moved off ECS Fargate. "
             "Terraform replaced CloudFormation for IaC."),
        ],
        "query": "infrastructure deployment platform",
        "query_type": "keyword",
        "expected": ["EKS", "Kubernetes", "Terraform"],
        "negative": [],
    })

    questions.append({
        "question_id": "KU-004",
        "category": "KU",
        "description": "Updated fact: database version upgrade",
        "sessions": [
            ("2025-01-01T10:00:00Z",
             "Production database is PostgreSQL 14.8 running on RDS with "
             "a db.r6g.2xlarge instance."),
            ("2025-06-15T10:00:00Z",
             "The PostgreSQL upgrade to version 16.2 was completed during "
             "the maintenance window. The instance was also resized to "
             "db.r7g.2xlarge for better price-performance."),
        ],
        "query": "PostgreSQL version production",
        "query_type": "keyword",
        "expected": ["16.2"],
        "negative": [],
    })

    questions.append({
        "question_id": "KU-005",
        "category": "KU",
        "description": "Updated preference: CI/CD platform change",
        "sessions": [
            ("2025-02-01T10:00:00Z",
             "The CI/CD pipeline runs on Jenkins with a custom Groovy "
             "pipeline library. Build times average 12 minutes."),
            ("2025-05-01T10:00:00Z",
             "The CI/CD pipeline was migrated from Jenkins to GitHub Actions. "
             "Build times dropped to 4 minutes using the new matrix strategy "
             "with self-hosted runners on ARM64 instances."),
        ],
        "query": "CI/CD pipeline",
        "query_type": "keyword",
        "expected": ["GitHub Actions"],
        "negative": [],
    })

    # =========================================================================
    # Category 4: Temporal Reasoning (TR)
    # Queries requiring awareness of when things happened
    # =========================================================================

    questions.append({
        "question_id": "TR-001",
        "category": "TR",
        "description": "Temporal filter: events after a specific date",
        "sessions": [
            ("2025-03-15T10:00:00Z",
             "Deployed v2.1.0 of the payment service with Stripe Connect "
             "support for marketplace sellers."),
            ("2025-06-20T10:00:00Z",
             "Deployed v3.0.0 of the payment service with Apple Pay and "
             "Google Pay integration. This was a major release."),
            ("2025-09-01T10:00:00Z",
             "Deployed v3.2.0 of the payment service with cryptocurrency "
             "payment support via Coinbase Commerce."),
        ],
        "query": "payment service deployment",
        "query_type": "temporal",
        "temporal_args": {"since": "2025-06-01T00:00:00Z"},
        "expected": ["v3.0.0", "v3.2.0"],
        "negative": ["v2.1.0"],
    })

    questions.append({
        "question_id": "TR-002",
        "category": "TR",
        "description": "Temporal filter: events in a date range",
        "sessions": [
            ("2025-01-10T10:00:00Z",
             "Q1 sprint planning: focus on payment processing refactor and "
             "new onboarding flow."),
            ("2025-04-05T10:00:00Z",
             "Q2 sprint planning: priorities are API versioning strategy "
             "and mobile app redesign."),
            ("2025-07-02T10:00:00Z",
             "Q3 sprint planning: main goals are observability improvements "
             "and database sharding for the analytics service."),
            ("2025-10-01T10:00:00Z",
             "Q4 sprint planning: focus on annual security audit preparation "
             "and SOC2 compliance automation."),
        ],
        "query": "sprint planning priorities",
        "query_type": "temporal",
        "temporal_args": {"since": "2025-04-01T00:00:00Z",
                          "until": "2025-08-01T00:00:00Z"},
        "expected": ["API versioning", "observability"],
        "negative": ["payment processing", "security audit"],
    })

    questions.append({
        "question_id": "TR-003",
        "category": "TR",
        "description": "Temporal filter: most recent event only",
        "sessions": [
            ("2025-02-01T10:00:00Z",
             "The API rate limit was set to 100 requests per second per "
             "client as the initial conservative value."),
            ("2025-05-01T10:00:00Z",
             "After load testing, the API rate limit was increased to 500 "
             "requests per second per client."),
            ("2025-08-01T10:00:00Z",
             "Following the enterprise tier launch, premium clients got a "
             "rate limit of 2000 requests per second while free tier stayed "
             "at 500."),
        ],
        "query": "API rate limit configuration",
        "query_type": "temporal",
        "temporal_args": {"since": "2025-07-01T00:00:00Z"},
        "expected": ["2000"],
        "negative": ["100 requests per second per client as the initial"],
    })

    questions.append({
        "question_id": "TR-004",
        "category": "TR",
        "description": "Temporal: order of events matters",
        "sessions": [
            ("2025-03-01T10:00:00Z",
             "Incident INC-2025-031: Memory leak in the recommendation "
             "service caused OOM kills. Hotfix deployed same day."),
            ("2025-05-15T10:00:00Z",
             "Incident INC-2025-052: Database failover triggered by network "
             "partition between AZ-1 and AZ-2. RTO was 45 seconds."),
            ("2025-08-22T10:00:00Z",
             "Incident INC-2025-083: CDN cache poisoning through crafted "
             "Host headers. WAF rule added within 2 hours of detection."),
        ],
        "query": "production incident",
        "query_type": "temporal",
        "temporal_args": {"since": "2025-05-01T00:00:00Z",
                          "until": "2025-06-01T00:00:00Z"},
        "expected": ["INC-2025-052", "failover"],
        "negative": ["INC-2025-031", "INC-2025-083"],
    })

    questions.append({
        "question_id": "TR-005",
        "category": "TR",
        "description": "Temporal: recent changes query",
        "sessions": [
            ("2025-09-25T10:00:00Z",
             "Added OpenTelemetry tracing to the checkout flow. Traces are "
             "exported to Jaeger running on the observability cluster."),
            ("2025-09-28T10:00:00Z",
             "Migrated the user preferences API from REST to GraphQL using "
             "Apollo Server 4. The REST endpoint is deprecated but still "
             "available for 90 days."),
            ("2025-10-01T10:00:00Z",
             "Rolled out feature flags for the new recommendation engine "
             "using LaunchDarkly. Currently enabled for 10 percent of users."),
        ],
        "query": "recent changes",
        "query_type": "temporal",
        "temporal_args": {"since": "2025-09-27T00:00:00Z"},
        "expected": ["GraphQL", "LaunchDarkly"],
        "negative": [],
    })

    # =========================================================================
    # Category 5: Abstention (ABS)
    # Should return empty/no results for questions about non-existent info
    # =========================================================================

    questions.append({
        "question_id": "ABS-001",
        "category": "ABS",
        "description": "False premise: technology never discussed",
        "sessions": [
            ("2025-06-01T10:00:00Z",
             "The backend API is built with Go and uses the chi router. "
             "Deployment is automated via GitHub Actions."),
        ],
        "query": "Ruby on Rails migration plan",
        "query_type": "keyword",
        "expected": [],
        "negative": ["Rails", "Ruby"],
    })

    questions.append({
        "question_id": "ABS-002",
        "category": "ABS",
        "description": "False premise: person never mentioned",
        "sessions": [
            ("2025-06-10T10:00:00Z",
             "Alice is the tech lead and Bob is the SRE. They work together "
             "on the infrastructure migration project."),
        ],
        "query": "David's role on the mobile team",
        "query_type": "keyword",
        "expected": [],
        "negative": ["David"],
    })

    questions.append({
        "question_id": "ABS-003",
        "category": "ABS",
        "description": "False premise: event that never happened",
        "sessions": [
            ("2025-07-01T10:00:00Z",
             "The staging environment was migrated to the new VPC. Production "
             "migration is scheduled for next quarter."),
        ],
        "query": "Terraform module refactoring timeline",
        "query_type": "keyword",
        "expected": [],
        "negative": [],
    })

    questions.append({
        "question_id": "ABS-004",
        "category": "ABS",
        "description": "False premise: unrelated domain query",
        "sessions": [
            ("2025-05-01T10:00:00Z",
             "The e-commerce platform processes about 50000 orders per day "
             "with an average order value of 85 dollars."),
        ],
        "query": "machine learning model training hyperparameters",
        "query_type": "keyword",
        "expected": [],
        "negative": ["hyperparameters", "training"],
    })

    questions.append({
        "question_id": "ABS-005",
        "category": "ABS",
        "description": "False premise: future event queried as past",
        "sessions": [
            ("2025-06-01T10:00:00Z",
             "The SOC2 audit is planned for Q1 2026. Preparation work will "
             "begin in November 2025."),
        ],
        "query": "SOC2 audit findings and remediation results",
        "query_type": "keyword",
        "expected": [],
        "negative": ["findings", "remediation"],
    })

    return questions


def get_categories():
    """Return category metadata."""
    return {
        "IE": {
            "name": "Information Extraction",
            "description": "Recall specific facts from stored memories",
            "count": 8,
        },
        "MR": {
            "name": "Multi-Session Reasoning",
            "description": "Synthesize information across multiple memories",
            "count": 6,
        },
        "KU": {
            "name": "Knowledge Updates",
            "description": "Track evolving information over time",
            "count": 5,
        },
        "TR": {
            "name": "Temporal Reasoning",
            "description": "Answer questions requiring time awareness",
            "count": 5,
        },
        "ABS": {
            "name": "Abstention",
            "description": "Correctly refuse when information is unavailable",
            "count": 5,
        },
    }


if __name__ == "__main__":
    dataset = generate_dataset()
    cats = get_categories()
    print(f"LongMemEval synthetic dataset: {len(dataset)} questions")
    for cat_id, cat in cats.items():
        actual = sum(1 for q in dataset if q["category"] == cat_id)
        print(f"  {cat_id}: {cat['name']:<30s} {actual} questions")
