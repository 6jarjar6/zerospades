/*
 Copyright (c) 2013 yvt

 This file is part of OpenSpades.

 OpenSpades is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 OpenSpades is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with OpenSpades.  If not, see <http://www.gnu.org/licenses/>.

 */

uniform mat4 projectionViewMatrix;
uniform mat4 viewMatrix;
uniform vec3 chunkPosition;
uniform vec3 viewOriginVector;

// [x, y, z]
attribute vec3 positionAttribute;

// [R, G, B, diffuse]
attribute vec4 colorAttribute;

// [nx, ny, nz]
attribute vec3 normalAttribute;

varying vec4 color;
varying vec3 fogDensity;

void PrepareForDynamicLightNoBump(vec3 vertexCoord, vec3 normal);
vec4 ComputeFogDensity(float poweredLength);

void main() {
	vec4 vertexPos = vec4(chunkPosition + positionAttribute, 1.0);

	gl_Position = projectionViewMatrix * vertexPos;

	color = colorAttribute;
	color.xyz *= color.xyz; // linearize

	vec2 horzRelativePos = vertexPos.xy - viewOriginVector.xy;
	float horzDistance = dot(horzRelativePos, horzRelativePos);
	fogDensity = ComputeFogDensity(horzDistance).xyz;

	PrepareForDynamicLightNoBump(vertexPos.xyz, normalAttribute);
}