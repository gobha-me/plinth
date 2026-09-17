{{/* SPDX-License-Identifier: MIT */}}
{{- define "plinth.name" -}}
{{- .Chart.Name | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "plinth.fullname" -}}
{{- $name := include "plinth.name" . -}}
{{- if contains $name .Release.Name -}}
{{- .Release.Name | trunc 48 | trimSuffix "-" -}}
{{- else -}}
{{- printf "%s-%s" .Release.Name $name | trunc 48 | trimSuffix "-" -}}
{{- end -}}
{{- end -}}

{{- define "plinth.chart" -}}
{{- printf "%s-%s" .Chart.Name .Chart.Version | replace "+" "_" | trunc 63 | trimSuffix "-" -}}
{{- end -}}

{{- define "plinth.selectorLabels" -}}
app.kubernetes.io/name: {{ include "plinth.name" . }}
app.kubernetes.io/instance: {{ .Release.Name | quote }}
{{- end -}}

{{- define "plinth.labels" -}}
helm.sh/chart: {{ include "plinth.chart" . }}
{{ include "plinth.selectorLabels" . }}
app.kubernetes.io/managed-by: {{ .Release.Service | quote }}
{{- end -}}

{{- define "plinth.image" -}}
{{- $repository := required "image.repository is required" .Values.image.repository -}}
{{- $digest := required "image.digest must be the sha256 digest recorded by the release workflow" .Values.image.digest -}}
{{- if contains "@" $repository -}}
{{- fail "image.repository must not contain a digest; set image.digest separately" -}}
{{- end -}}
{{- if regexMatch ":[^/]+$" $repository -}}
{{- fail "image.repository must not contain a tag; the chart accepts digest identity only" -}}
{{- end -}}
{{- if not (regexMatch "^sha256:[0-9a-f]{64}$" $digest) -}}
{{- fail "image.digest must be sha256 followed by 64 lowercase hexadecimal characters" -}}
{{- end -}}
{{- printf "%s@%s" $repository $digest -}}
{{- end -}}

{{- define "plinth.origin" -}}
{{- $host := required "public.host is required when traefik.enabled=true" .Values.public.host -}}
{{- if eq (int .Values.public.port) 443 -}}
{{- printf "https://%s" $host -}}
{{- else -}}
{{- printf "https://%s:%d" $host (int .Values.public.port) -}}
{{- end -}}
{{- end -}}

{{- define "plinth.dataClaimName" -}}
{{- default (printf "%s-data" (include "plinth.fullname" .)) .Values.persistence.data.existingClaim -}}
{{- end -}}

{{- define "plinth.logsClaimName" -}}
{{- default (printf "%s-logs" (include "plinth.fullname" .)) .Values.persistence.logs.existingClaim -}}
{{- end -}}

{{- define "plinth.traefikService" -}}
kind: Service
name: {{ include "plinth.fullname" . }}
port: http
scheme: http
passHostHeader: true
serversTransport: {{ include "plinth.fullname" . }}
{{- end -}}

{{- define "plinth.traefikPackageService" -}}
kind: Service
name: {{ include "plinth.fullname" . }}
port: http
scheme: http
passHostHeader: true
serversTransport: {{ include "plinth.fullname" . }}-package
{{- end -}}

{{- define "plinth.traefikTLS" -}}
secretName: {{ required "traefik.tls.secretName is required when traefik.enabled=true" .Values.traefik.tls.secretName | quote }}
{{- end -}}
