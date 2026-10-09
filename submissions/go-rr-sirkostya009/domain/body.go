package domain

import "errors"

// ErrBodyNotString is a body key holding valid JSON that is not a string.
var ErrBodyNotString = errors.New("body is not a string")

// anyObject takes any JSON object and keeps nothing of it.
//
//ggen:generate ignoreunknown allowdups novalidate
type anyObject struct{}

// The converters behind CreatePost.Body's non-string shapes. Reaching one
// means the value parsed, so its error marks a wrong type, never bad syntax.
func numberBody(float64) (string, error)   { return "", ErrBodyNotString }
func boolBody(bool) (string, error)        { return "", ErrBodyNotString }
func objectBody(anyObject) (string, error) { return "", ErrBodyNotString }
