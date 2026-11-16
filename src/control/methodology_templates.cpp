// SPDX-License-Identifier: MIT

// The Python templates a methodology draft starts from (docs/api/methodologies.md#drafts,
// lake#655). Standard library only, so a draft's lock file is empty and its image installs nothing.
// Each is the helpers below, then one template's fit, predict and score (and explain where it can).
// The tests run all four through the interface with python3 when it is on the path.

#include "methodology_templates.hpp"

namespace lake::control::methodology_templates {

const std::string_view helpers = R"PY(import math
import random


def _numbers(rows):
    """Each row's feature values as floats; a value that is not a number counts as 0."""
    out = []
    for row in rows:
        values = []
        for value in row:
            if isinstance(value, bool):
                values.append(1.0 if value else 0.0)
                continue
            try:
                number = float(value)
            except (TypeError, ValueError):
                number = 0.0
            values.append(number if math.isfinite(number) else 0.0)
        out.append(values)
    return out


def _scale(rows, stats=None):
    """Standardizes each column with the training rows' mean and spread (kept in the artifact)."""
    if stats is None:
        count = max(len(rows), 1)
        width = len(rows[0]) if rows else 0
        means = [sum(row[j] for row in rows) / count for j in range(width)]
        spreads = []
        for j in range(width):
            variance = sum((row[j] - means[j]) ** 2 for row in rows) / count
            spreads.append(math.sqrt(variance) or 1.0)
        stats = {"means": means, "spreads": spreads}
    means, spreads = stats["means"], stats["spreads"]
    return [[(row[j] - means[j]) / spreads[j] for j in range(len(means))] for row in rows], stats
)PY";

const std::string_view binary = R"PY(def _label(value):
    """A yes-or-no target as 1.0 or 0.0: true, yes, 1 and any non-zero number are 1."""
    if isinstance(value, str):
        return 1.0 if value.strip().lower() in ("1", "1.0", "true", "yes", "y", "t") else 0.0
    try:
        return 1.0 if float(value) != 0 else 0.0
    except (TypeError, ValueError):
        return 0.0


def _sigmoid(z):
    if z < -35:
        return 0.0
    if z > 35:
        return 1.0
    return 1.0 / (1.0 + math.exp(-z))


def fit(data, params):
    """Logistic regression fitted by full-batch gradient descent on standardized features."""
    rows, stats = _scale(_numbers(data.X()))
    labels = [_label(value) for value in data.y()]
    rate = float(params.get("learning_rate", 0.1))
    epochs = int(params.get("epochs", 200))
    l2 = float(params.get("l2", 0.0))
    width = len(rows[0]) if rows else 0
    weights = [0.0] * width
    bias = 0.0
    count = max(len(rows), 1)
    for _ in range(epochs):
        gradient = [0.0] * width
        gradient_bias = 0.0
        for row, label in zip(rows, labels):
            error = _sigmoid(bias + sum(w * x for w, x in zip(weights, row))) - label
            for j in range(width):
                gradient[j] += error * row[j]
            gradient_bias += error
        for j in range(width):
            weights[j] -= rate * (gradient[j] / count + l2 * weights[j])
        bias -= rate * gradient_bias / count
    return {"weights": weights, "bias": bias, "scale": stats, "features": list(data.features)}


def predict(artifact, data):
    """The probability of the positive outcome for each row."""
    rows, _ = _scale(_numbers(data.X()), artifact["scale"])
    return [_sigmoid(artifact["bias"] + sum(w * x for w, x in zip(artifact["weights"], row)))
            for row in rows]


def _auc(scores, labels):
    """The area under the ROC curve from ranks (ties share their average rank)."""
    order = sorted(range(len(scores)), key=lambda index: scores[index])
    ranks = [0.0] * len(scores)
    start = 0
    while start < len(order):
        end = start
        while end + 1 < len(order) and scores[order[end + 1]] == scores[order[start]]:
            end += 1
        for position in range(start, end + 1):
            ranks[order[position]] = (start + end) / 2.0 + 1.0
        start = end + 1
    positives = sum(1 for label in labels if label == 1.0)
    negatives = len(labels) - positives
    if positives == 0 or negatives == 0:
        return 0.5
    rank_sum = sum(rank for rank, label in zip(ranks, labels) if label == 1.0)
    return (rank_sum - positives * (positives + 1) / 2.0) / (positives * negatives)


def score(artifact, data):
    probabilities = predict(artifact, data)
    labels = [_label(value) for value in data.y()]
    count = max(len(labels), 1)
    epsilon = 1e-12
    log_loss = -sum(label * math.log(max(p, epsilon)) + (1 - label) * math.log(max(1 - p, epsilon))
                    for p, label in zip(probabilities, labels)) / count
    accuracy = sum(1 for p, label in zip(probabilities, labels) if (p >= 0.5) == (label == 1.0)) / count
    return {"auc": _auc(probabilities, labels), "log_loss": log_loss, "accuracy": accuracy}


def explain(artifact, data):
    """Each feature's contribution to each row's log-odds: its weight times its scaled value."""
    rows, _ = _scale(_numbers(data.X()), artifact["scale"])
    names = artifact["features"]
    return [{name: w * x for name, w, x in zip(names, artifact["weights"], row)} for row in rows]
)PY";

const std::string_view multiclass = R"PY(def _classes(values):
    return sorted({str(value) for value in values})


def _softmax(scores):
    top = max(scores)
    exps = [math.exp(score - top) for score in scores]
    total = sum(exps)
    return [value / total for value in exps]


def fit(data, params):
    """Multinomial logistic regression fitted by full-batch gradient descent."""
    rows, stats = _scale(_numbers(data.X()))
    targets = [str(value) for value in data.y()]
    classes = _classes(targets)
    index = {name: position for position, name in enumerate(classes)}
    rate = float(params.get("learning_rate", 0.1))
    epochs = int(params.get("epochs", 200))
    l2 = float(params.get("l2", 0.0))
    width = len(rows[0]) if rows else 0
    weights = [[0.0] * width for _ in classes]
    biases = [0.0] * len(classes)
    count = max(len(rows), 1)
    for _ in range(epochs):
        gradient = [[0.0] * width for _ in classes]
        gradient_bias = [0.0] * len(classes)
        for row, target in zip(rows, targets):
            probabilities = _softmax([b + sum(w * x for w, x in zip(ws, row))
                                      for ws, b in zip(weights, biases)])
            for k, probability in enumerate(probabilities):
                error = probability - (1.0 if index[target] == k else 0.0)
                for j in range(width):
                    gradient[k][j] += error * row[j]
                gradient_bias[k] += error
        for k in range(len(classes)):
            for j in range(width):
                weights[k][j] -= rate * (gradient[k][j] / count + l2 * weights[k][j])
            biases[k] -= rate * gradient_bias[k] / count
    return {"classes": classes, "weights": weights, "biases": biases, "scale": stats}


def predict(artifact, data):
    """The most likely class for each row."""
    rows, _ = _scale(_numbers(data.X()), artifact["scale"])
    out = []
    for row in rows:
        scores = [b + sum(w * x for w, x in zip(ws, row))
                  for ws, b in zip(artifact["weights"], artifact["biases"])]
        out.append(artifact["classes"][scores.index(max(scores))])
    return out


def score(artifact, data):
    predicted = predict(artifact, data)
    actual = [str(value) for value in data.y()]
    count = max(len(actual), 1)
    accuracy = sum(1 for p, a in zip(predicted, actual) if p == a) / count
    f1_scores = []
    for name in sorted(set(actual) | set(predicted)):
        true_positive = sum(1 for p, a in zip(predicted, actual) if p == name and a == name)
        predicted_count = sum(1 for p in predicted if p == name)
        actual_count = sum(1 for a in actual if a == name)
        precision = true_positive / predicted_count if predicted_count else 0.0
        recall = true_positive / actual_count if actual_count else 0.0
        f1_scores.append(2 * precision * recall / (precision + recall) if precision + recall else 0.0)
    macro_f1 = sum(f1_scores) / len(f1_scores) if f1_scores else 0.0
    return {"accuracy": accuracy, "macro_f1": macro_f1}
)PY";

const std::string_view regression = R"PY(def _target(value):
    try:
        number = float(value)
    except (TypeError, ValueError):
        return 0.0
    return number if math.isfinite(number) else 0.0


def fit(data, params):
    """Linear regression fitted by full-batch gradient descent on standardized features."""
    rows, stats = _scale(_numbers(data.X()))
    targets = [_target(value) for value in data.y()]
    count = max(len(rows), 1)
    mean = sum(targets) / count
    spread = math.sqrt(sum((t - mean) ** 2 for t in targets) / count) or 1.0
    scaled = [(t - mean) / spread for t in targets]
    rate = float(params.get("learning_rate", 0.1))
    epochs = int(params.get("epochs", 200))
    l2 = float(params.get("l2", 0.0))
    width = len(rows[0]) if rows else 0
    weights = [0.0] * width
    bias = 0.0
    for _ in range(epochs):
        gradient = [0.0] * width
        gradient_bias = 0.0
        for row, target in zip(rows, scaled):
            error = bias + sum(w * x for w, x in zip(weights, row)) - target
            for j in range(width):
                gradient[j] += error * row[j]
            gradient_bias += error
        for j in range(width):
            weights[j] -= rate * (gradient[j] / count + l2 * weights[j])
        bias -= rate * gradient_bias / count
    return {"weights": weights, "bias": bias, "scale": stats, "target": {"mean": mean, "spread": spread},
            "features": list(data.features)}


def predict(artifact, data):
    rows, _ = _scale(_numbers(data.X()), artifact["scale"])
    target = artifact["target"]
    return [target["mean"] + target["spread"] * (artifact["bias"] + sum(w * x for w, x in zip(artifact["weights"], row)))
            for row in rows]


def score(artifact, data):
    predicted = predict(artifact, data)
    actual = [_target(value) for value in data.y()]
    count = max(len(actual), 1)
    errors = [p - a for p, a in zip(predicted, actual)]
    mean = sum(actual) / count
    total = sum((a - mean) ** 2 for a in actual)
    residual = sum(e ** 2 for e in errors)
    return {"rmse": math.sqrt(residual / count), "mae": sum(abs(e) for e in errors) / count,
            "r2": 1.0 - residual / total if total else 0.0}


def explain(artifact, data):
    """Each feature's contribution to each row's prediction, in the target's units."""
    rows, _ = _scale(_numbers(data.X()), artifact["scale"])
    spread = artifact["target"]["spread"]
    names = artifact["features"]
    return [{name: spread * w * x for name, w, x in zip(names, artifact["weights"], row)} for row in rows]
)PY";

const std::string_view clustering = R"PY(def _distance(left, right):
    return sum((a - b) ** 2 for a, b in zip(left, right))


def _nearest(centers, row):
    distances = [_distance(center, row) for center in centers]
    return distances.index(min(distances))


def fit(data, params):
    """k-means on standardized features, started from k-means++ seeds with a fixed seed."""
    rows, stats = _scale(_numbers(data.X()))
    k = max(1, min(int(params.get("clusters", 4)), len(rows) or 1))
    iterations = int(params.get("iterations", 50))
    generator = random.Random(int(params.get("seed", 42)))
    centers = [list(rows[generator.randrange(len(rows))])] if rows else [[]]
    while len(centers) < k:
        weights = [min(_distance(center, row) for center in centers) for row in rows]
        total = sum(weights)
        if total == 0:
            break
        pick = generator.random() * total
        running = 0.0
        for row, weight in zip(rows, weights):
            running += weight
            if running >= pick:
                centers.append(list(row))
                break
    for _ in range(iterations):
        groups = [[] for _ in centers]
        for row in rows:
            groups[_nearest(centers, row)].append(row)
        moved = False
        for index, group in enumerate(groups):
            if not group:
                continue
            center = [sum(values) / len(group) for values in zip(*group)]
            if center != centers[index]:
                centers[index] = center
                moved = True
        if not moved:
            break
    return {"centers": centers, "scale": stats}


def predict(artifact, data):
    """The cluster each row is nearest to."""
    rows, _ = _scale(_numbers(data.X()), artifact["scale"])
    return [_nearest(artifact["centers"], row) for row in rows]


def score(artifact, data):
    rows, _ = _scale(_numbers(data.X()), artifact["scale"])
    count = max(len(rows), 1)
    assigned = [_nearest(artifact["centers"], row) for row in rows]
    inertia = sum(_distance(artifact["centers"][cluster], row) for cluster, row in zip(assigned, rows)) / count
    sizes = [assigned.count(index) for index in range(len(artifact["centers"]))]
    return {"inertia": inertia, "largest_cluster_share": max(sizes) / count if sizes else 0.0,
            "clusters": float(sum(1 for size in sizes if size))}
)PY";

} // namespace lake::control::methodology_templates
