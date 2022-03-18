/*
 Copyright (c) 2017 yvt

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

#include <vector>

#include "GLFXAAFilter.h"
#include "GLProfiler.h"
#include "GLProgram.h"
#include "GLProgramAttribute.h"
#include "GLProgramUniform.h"
#include "GLQuadRenderer.h"
#include "GLRenderer.h"
#include "GLTemporalAAFilter.h"
#include "IGLDevice.h"
#include <Core/Debug.h>
#include <Core/Math.h>

namespace spades {
	namespace draw {
		GLTemporalAAFilter::GLTemporalAAFilter(GLRenderer& renderer) : renderer(renderer) {
			prevMatrix = Matrix4::Identity();
			prevViewOrigin = Vector3(0.0F, 0.0F, 0.0F);
			program = renderer.RegisterProgram("Shaders/PostFilters/TemporalAA.program");

			// Preload
			GLFXAAFilter{renderer};
		}

		GLTemporalAAFilter::~GLTemporalAAFilter() { DeleteHistoryBuffer(); }

		void GLTemporalAAFilter::DeleteHistoryBuffer() {
			if (!historyBuffer.valid)
				return;

			IGLDevice& dev = renderer.GetGLDevice();
			dev.DeleteFramebuffer(historyBuffer.framebuffer);
			dev.DeleteTexture(historyBuffer.texture);

			historyBuffer.valid = false;
		}

		GLColorBuffer GLTemporalAAFilter::Filter(GLColorBuffer input, bool useFxaa) {
			SPADES_MARK_FUNCTION();

			IGLDevice& dev = renderer.GetGLDevice();
			GLQuadRenderer qr(dev);

			// Calculate the current view-projection matrix.
			const client::SceneDefinition& def = renderer.GetSceneDef();
			Matrix4 newMatrix = Matrix4::Identity();
			Vector3 axes[] = {def.viewAxis[0], def.viewAxis[1], def.viewAxis[2]};
			newMatrix.m[0] = axes[0].x;
			newMatrix.m[1] = axes[1].x;
			newMatrix.m[2] = -axes[2].x;
			newMatrix.m[4] = axes[0].y;
			newMatrix.m[5] = axes[1].y;
			newMatrix.m[6] = -axes[2].y;
			newMatrix.m[8] = axes[0].z;
			newMatrix.m[9] = axes[1].z;
			newMatrix.m[10] = -axes[2].z;

			Matrix4 projectionMatrix;
			{
				// From `GLRenderer::BuildProjectionMatrix`
				float near = def.zNear;
				float far = def.zFar;
				float t = near * std::tan(def.fovY * 0.5F);
				float r = near * std::tan(def.fovX * 0.5F);
				float a = r * 2.0F, b = t * 2.0F, c = far - near;
				Matrix4& mat = projectionMatrix;
				mat.m[0] = near * 2.0F / a;
				mat.m[1] = 0.0F;
				mat.m[2] = 0.0F;
				mat.m[3] = 0.0F;
				mat.m[4] = 0.0F;
				mat.m[5] = near * 2.0F / b;
				mat.m[6] = 0.0F;
				mat.m[7] = 0.0F;
				mat.m[8] = 0.0F;
				mat.m[9] = 0.0F;
				mat.m[10] = -(far + near) / c;
				mat.m[11] = -1.0F;
				mat.m[12] = 0.0F;
				mat.m[13] = 0.0F;
				mat.m[14] = -(far * near * 2.0F) / c;
				mat.m[15] = 0.0F;
			}

			newMatrix = projectionMatrix * newMatrix;

			// In `y = newMatrix * x`, the coordinate space `y` belongs to must
			// cover the clip region by range `[0, 1]` (like texture coordinates)
			// instead of `[-1, 1]` (like OpenGL clip coordinates)
			newMatrix = Matrix4::Translate(1.0F, 1.0F, 1.0F) * newMatrix;
			newMatrix = Matrix4::Scale(0.5F, 0.5F, 0.5F) * newMatrix;

			// Camera translation must be incorporated into the calculation
			// separately to avoid numerical errors. (You'd be suprised to see
			// how visible the visual artifacts can be.)
			Matrix4 translationMatrix = Matrix4::Translate(def.viewOrigin - prevViewOrigin);

			// Compute the reprojection matrix
			Matrix4 inverseNewMatrix = newMatrix.Inversed();
			Matrix4 diffMatrix = prevMatrix * translationMatrix * inverseNewMatrix;
			prevMatrix = newMatrix;
			prevViewOrigin = def.viewOrigin;

			if (!historyBuffer.valid || historyBuffer.width != input.GetWidth() ||
			    historyBuffer.height != input.GetHeight()) {
				DeleteHistoryBuffer();

				historyBuffer.width = input.GetWidth();
				historyBuffer.height = input.GetHeight();

				auto internalFormat = renderer.GetFramebufferManager()->GetMainInternalFormat();

				historyBuffer.framebuffer = dev.GenFramebuffer();
				dev.BindFramebuffer(IGLDevice::Framebuffer, historyBuffer.framebuffer);

				historyBuffer.texture = dev.GenTexture();
				dev.BindTexture(IGLDevice::Texture2D, historyBuffer.texture);

				historyBuffer.valid = true;

				SPLog("Creating a history buffer");
				dev.TexImage2D(IGLDevice::Texture2D, 0, internalFormat, historyBuffer.width,
				               historyBuffer.height, 0, IGLDevice::RGBA, IGLDevice::UnsignedByte,
				               NULL);

				SPLog("History buffer allocated");
				dev.TexParamater(IGLDevice::Texture2D, IGLDevice::TextureMagFilter, IGLDevice::Linear);
				dev.TexParamater(IGLDevice::Texture2D, IGLDevice::TextureMinFilter, IGLDevice::Linear);
				dev.TexParamater(IGLDevice::Texture2D, IGLDevice::TextureWrapS, IGLDevice::ClampToEdge);
				dev.TexParamater(IGLDevice::Texture2D, IGLDevice::TextureWrapT, IGLDevice::ClampToEdge);

				dev.FramebufferTexture2D(IGLDevice::Framebuffer, IGLDevice::ColorAttachment0,
				                         IGLDevice::Texture2D, historyBuffer.texture, 0);

				IGLDevice::Enum status = dev.CheckFramebufferStatus(IGLDevice::Framebuffer);
				if (status != IGLDevice::FramebufferComplete)
					SPRaise("Failed to create a history buffer.");
				SPLog("Created a history framebuffer");

				// Initialize the history buffer with the latest input
				dev.BindFramebuffer(IGLDevice::DrawFramebuffer, historyBuffer.framebuffer);
				dev.BindFramebuffer(IGLDevice::ReadFramebuffer, input.GetFramebuffer());
				dev.BlitFramebuffer(0, 0, input.GetWidth(), input.GetHeight(), 0, 0,
				                    input.GetWidth(), input.GetHeight(), IGLDevice::ColorBufferBit,
				                    IGLDevice::Nearest);
				dev.BindFramebuffer(IGLDevice::ReadFramebuffer, 0);
				dev.BindFramebuffer(IGLDevice::DrawFramebuffer, 0);

				// Reset the blending factor
				dev.BindFramebuffer(IGLDevice::Framebuffer, historyBuffer.framebuffer);
				dev.ColorMask(false, false, false, true);
				dev.ClearColor(0.0F, 0.0F, 0.0F, 0.5F);
				dev.Clear(IGLDevice::ColorBufferBit);
				dev.ColorMask(true, true, true, true);

				if (useFxaa)
					return GLFXAAFilter{renderer}.Filter(input);

				return input;
			}

			GLColorBuffer output = input.GetManager()->CreateBufferHandle();
			GLColorBuffer processedInput = input;

			static GLProgramAttribute positionAttribute("positionAttribute");
			static GLProgramUniform inputTexture("inputTexture");
			static GLProgramUniform depthTexture("depthTexture");
			static GLProgramUniform previousTexture("previousTexture");
			static GLProgramUniform processedInputTexture("processedInputTexture");
			static GLProgramUniform reprojectionMatrix("reprojectionMatrix");
			static GLProgramUniform inverseVP("inverseVP");
			static GLProgramUniform viewProjectionMatrixInv("viewProjectionMatrixInv");
			static GLProgramUniform fogDistance("fogDistance");

			dev.Enable(IGLDevice::Blend, false);

			positionAttribute(program);
			inputTexture(program);
			depthTexture(program);
			previousTexture(program);
			processedInputTexture(program);
			reprojectionMatrix(program);
			inverseVP(program);
			viewProjectionMatrixInv(program);
			fogDistance(program);

			program->Use();

			inputTexture.SetValue(0);
			previousTexture.SetValue(1);
			processedInputTexture.SetValue(2);
			depthTexture.SetValue(3);
			reprojectionMatrix.SetValue(diffMatrix);
			inverseVP.SetValue(1.0F / input.GetWidth(), 1.0F / input.GetHeight());
			viewProjectionMatrixInv.SetValue(inverseNewMatrix);
			fogDistance.SetValue(128.f);

			// Perform temporal AA
			// TODO: pre/post tone mapping to prevent aliasing near overbright area
			qr.SetCoordAttributeIndex(positionAttribute());
			dev.ActiveTexture(0);
			dev.BindTexture(IGLDevice::Texture2D, input.GetTexture());
			dev.ActiveTexture(1);
			dev.BindTexture(IGLDevice::Texture2D, historyBuffer.texture);
			dev.ActiveTexture(2);
			dev.BindTexture(IGLDevice::Texture2D, processedInput.GetTexture());
			dev.ActiveTexture(3);
			dev.BindTexture(IGLDevice::Texture2D,
			                renderer.GetFramebufferManager()->GetDepthTexture());
			dev.ActiveTexture(0);
			dev.BindFramebuffer(IGLDevice::Framebuffer, output.GetFramebuffer());
			dev.Viewport(0, 0, output.GetWidth(), output.GetHeight());
			qr.Draw();
			dev.BindTexture(IGLDevice::Texture2D, 0);

			// Copy the result to the history buffer
			dev.BindFramebuffer(IGLDevice::DrawFramebuffer, historyBuffer.framebuffer);
			dev.BindFramebuffer(IGLDevice::ReadFramebuffer, output.GetFramebuffer());
			dev.BlitFramebuffer(0, 0, input.GetWidth(), input.GetHeight(), 0, 0, input.GetWidth(),
			                    input.GetHeight(), IGLDevice::ColorBufferBit, IGLDevice::Nearest);
			dev.BindFramebuffer(IGLDevice::ReadFramebuffer, 0);
			dev.BindFramebuffer(IGLDevice::DrawFramebuffer, 0);
			return output;
		}

		Vector2 GLTemporalAAFilter::GetProjectionMatrixJitter() {
			// Obtained from Hyper3D (MIT licensed)
			static const std::vector<float> jitterTable = {
			  0.281064F,    0.645281F,    -0.167313F,    0.685935F,    -0.160711F,    -0.113289F,
			  1.08453F,     -0.0970135F,  -0.3655F,      -0.51894F,    0.275308F,     -0.000830889F,
			  -0.0431051F,  0.574405F,    -0.163071F,    -0.30989F,    0.372959F,     -0.0161521F,
			  0.131741F,    0.456781F,    0.0165477F,    -0.0975113F,  -0.273682F,    -0.509164F,
			  0.573244F,    -0.714618F,   -0.479023F,    0.0525875F,   0.316595F,     -0.148211F,
			  -0.423713F,   -0.22462F,    -0.528986F,    0.390866F,    0.0439115F,    -0.274567F,
			  0.106133F,    -0.377686F,   0.481055F,     0.398664F,    0.314325F,     0.839894F,
			  -0.625382F,   0.0543475F,   -0.201899F,    0.198677F,    0.0182834F,    0.621111F,
			  0.128773F,    -0.265686F,   0.602337F,     0.296946F,    0.773769F,     0.0479956F,
			  -0.132997F,   -0.0410526F,  -0.254838F,    0.326185F,    0.347585F,     -0.580061F,
			  0.405482F,    0.101755F,    -0.201249F,    0.306534F,    0.469578F,     -0.111657F,
			  -0.796765F,   -0.0773768F,  -0.538891F,    0.206104F,    -0.0794146F,   0.098465F,
			  0.413728F,    0.0259771F,   -0.823897F,    0.0925169F,   0.88273F,      -0.184931F,
			  -0.134422F,   -0.247737F,   -0.682095F,    0.177566F,    0.299386F,     -0.329205F,
			  0.0488276F,   0.504052F,    0.268825F,     0.395508F,    -1.10225F,     0.101069F,
			  -0.0408943F,  -0.580797F,   -0.00804806F,  -0.402047F,   -0.418787F,    0.697977F,
			  -0.308492F,   -0.122199F,   0.628944F,     0.54588F,     0.0622768F,    -0.488552F,
			  0.0474367F,   0.215963F,    -0.679212F,    0.311237F,    -0.000920773F, -0.721814F,
			  0.579613F,    -0.0458724F,  -0.467233F,    0.268248F,    0.246741F,     -0.15576F,
			  0.0473638F,   0.0246596F,   -0.572414F,    -0.419131F,   -0.357526F,    0.452787F,
			  -0.112269F,   0.710673F,    -0.41551F,     0.429337F,    0.0882859F,    -0.433878F,
			  -0.0818105F,  -0.180361F,   0.36754F,      -0.49486F,    0.449489F,     -0.837214F,
			  -1.09047F,    0.168766F,    -0.163687F,    0.256186F,    0.633943F,     -0.012522F,
			  0.631576F,    -0.27161F,    -0.15392F,     -0.471082F,   -0.071748F,    -0.275351F,
			  -0.134404F,   0.126987F,    -0.478438F,    -0.144772F,   -0.38336F,     0.37449F,
			  -0.458729F,   -0.318997F,   -0.313852F,    0.081244F,    -0.287645F,    0.200266F,
			  -0.45997F,    0.108317F,    -0.216842F,    -0.165177F,   -0.296687F,    0.771041F,
			  0.933613F,    0.617833F,    -0.263007F,    -0.236543F,   -0.406302F,    0.241173F,
			  -0.225985F,   -0.108225F,   0.087069F,     -0.0444767F,  0.645569F,     -0.112983F,
			  -0.689477F,   0.498425F,    0.0738087F,    0.447277F,    0.0972104F,    -0.314627F,
			  0.393365F,    -0.0919185F,  -0.32199F,     -0.193414F,   -0.126091F,    0.185217F,
			  0.318475F,    0.140509F,    -0.115877F,    -0.911059F,   0.336104F,     -0.645395F,
			  0.00686884F,  -0.172296F,   -0.513633F,    -0.302956F,   -1.20699F,     0.148284F,
			  0.357629F,    0.58123F,     0.106886F,     -0.872183F,   -0.49183F,     -0.202535F,
			  -0.869357F,   0.0371933F,   -0.0869231F,   0.22624F,     0.198995F,     0.191016F,
			  0.151591F,    0.347114F,    0.056674F,     -0.213039F,   -0.228541F,    -0.473257F,
			  -0.574876F,   -0.0826995F,  -0.730448F,    0.343791F,    0.795006F,     0.366191F,
			  0.419235F,    -1.11688F,    0.227321F,     -0.0937171F,  0.156708F,     -0.3307F,
			  0.328026F,    -0.454046F,   0.432153F,     -0.189323F,   0.31821F,      0.312532F,
			  0.0963759F,   0.126471F,    -0.396326F,    0.0353236F,   -0.366891F,    -0.279321F,
			  0.106791F,    0.0697961F,   0.383726F,     0.260039F,    0.00297499F,   0.45812F,
			  -0.544967F,   -0.230453F,   -0.150821F,    -0.374241F,   -0.739835F,    0.462278F,
			  -0.76681F,    -0.455701F,   0.261229F,     0.274824F,    0.161605F,     -0.402379F,
			  0.571192F,    0.0844102F,   -0.47416F,     0.683535F,    0.144919F,     -0.134556F,
			  -0.0414159F,  0.357005F,    -0.643226F,    -0.00324917F, -0.173286F,    0.770447F,
			  0.261563F,    0.707628F,    0.131681F,     0.539707F,    -0.367105F,    0.150912F,
			  -0.310055F,   -0.270554F,   0.686523F,     0.195065F,    0.282361F,     0.569649F,
			  0.106642F,    0.296521F,    0.185682F,     0.124763F,    0.182832F,     0.42824F,
			  -0.489455F,   0.55954F,     0.383582F,     0.52804F,     -0.236162F,    -0.356153F,
			  0.70445F,     -0.300133F,   1.06101F,      0.0289559F,   0.4671F,       -0.0455821F,
			  -1.18106F,    0.26797F,     0.223324F,     0.793996F,    -0.833809F,    -0.412982F,
			  -0.443497F,   -0.634181F,   -0.000902414F, -0.319155F,   0.629076F,     -0.378669F,
			  -0.230422F,   0.489184F,    0.122302F,     0.397895F,    0.421496F,     -0.41475F,
			  0.192182F,    -0.477254F,   -0.32989F,     0.285264F,    -0.0248513F,   -0.224073F,
			  0.520192F,    0.138148F,    0.783388F,     0.540348F,    -0.468401F,    0.189778F,
			  0.327808F,    0.387399F,    0.0163817F,    0.340137F,    -0.174623F,    -0.560019F,
			  -0.32246F,    0.353305F,    0.513422F,     -0.472848F,   -0.0151656F,   0.0802364F,
			  -0.0833406F,  0.000303745F, -0.359159F,    -0.666926F,   0.446711F,     -0.254889F,
			  -0.263977F,   0.534997F,    0.555322F,     -0.315034F,   -0.62762F,     -0.14342F,
			  -0.78082F,    0.29739F,     0.0783401F,    -0.665565F,   -0.177726F,    0.62018F,
			  -0.723053F,   0.108446F,    0.550657F,     0.00324011F,  0.387362F,     -0.251661F,
			  -0.616413F,   -0.260163F,   -0.798613F,    0.0174665F,   -0.208833F,    -0.0398486F,
			  -0.506167F,   0.00121689F,  -0.75707F,     -0.0326216F,  0.30282F,      0.085227F,
			  -0.27267F,    0.25662F,     0.182456F,     -0.184061F,   -0.577699F,    -0.685311F,
			  0.587003F,    0.35393F,     -0.276868F,    -0.0617566F,  -0.365888F,    0.673723F,
			  -0.0476918F,  -0.0914235F,  0.560627F,     -0.387913F,   -0.194537F,    0.135256F,
			  -0.0808623F,  0.315394F,    -0.0383463F,   0.267406F,    0.545766F,     -0.659403F,
			  -0.410556F,   0.305285F,    0.0364261F,    0.396365F,    -0.284096F,    0.137003F,
			  0.611792F,    0.191185F,    0.440866F,     0.87738F,     0.470405F,     -0.372227F,
			  -0.84977F,    0.676291F,    -0.0709138F,   -0.456707F,   0.222892F,     -0.728947F,
			  0.2414F,      0.109269F,    0.707531F,     0.027894F,    -0.381266F,    -0.1872F,
			  -0.674006F,   -0.441284F,   -0.151681F,    -0.695721F,   0.360165F,     -0.397063F,
			  0.02772F,     0.271526F,    -0.170258F,    -0.198509F,   0.524165F,     0.29589F,
			  -0.895699F,   -0.266005F,   0.0971003F,    0.640709F,    -0.169635F,    0.0263381F,
			  -0.779951F,   -0.37692F,    -0.703577F,    0.00526047F,  -0.822414F,    -0.152364F,
			  0.10004F,     0.194787F,    0.453202F,     -0.495236F,   1.01192F,      -0.682168F,
			  -0.453866F,   0.387515F,    -0.355192F,    0.214262F,    0.2677F,       -0.263514F,
			  0.334733F,    0.683574F,    0.181592F,     0.599759F,    -0.182972F,    0.402297F,
			  -0.319075F,   0.553958F,    -0.990873F,    -0.143754F,   0.506054F,     0.0535431F,
			  -0.647583F,   0.53928F,     -0.510285F,    0.452258F,    -0.796479F,    0.186279F,
			  -0.0960782F,  -0.124537F,   0.509105F,     -0.1712F,     0.219554F,     -0.528307F,
			  -0.377211F,   -0.447177F,   -0.0283537F,   0.856948F,    -0.128052F,    0.482509F,
			  0.528981F,    -0.785958F,   0.816482F,     0.213728F,    -0.433917F,    -0.0413878F,
			  -0.997625F,   0.228201F,    -0.113198F,    0.425206F,    0.0261474F,    0.68678F,
			  0.224967F,    0.48489F,     0.53184F,      0.572936F,    -0.419627F,    -0.70428F,
			  -0.216836F,   0.57302F,     0.640487F,     -0.172722F,   0.237492F,     -0.390903F,
			  0.0717416F,   0.852097F,    -0.0422118F,   0.151465F,    -0.638427F,    0.132246F,
			  -0.0552788F,  0.436714F,    -0.281931F,    0.411517F,    -0.340499F,    -0.725834F,
			  -0.478547F,   0.332275F,    -0.0243354F,   -0.499295F,   0.238681F,     -0.324647F,
			  -0.182754F,   0.520306F,    -0.0762625F,   0.631812F,    -0.652095F,    -0.504378F,
			  -0.534564F,   0.118165F,    -0.384134F,    0.611485F,    0.635868F,     0.100705F,
			  0.25619F,     0.197184F,    0.328731F,     -0.0750947F,  -0.763023F,    0.516191F,
			  0.375317F,    -0.17778F,    0.880709F,     0.668956F,    0.376694F,     0.425053F,
			  -0.930982F,   0.0534644F,   -0.0423658F,   0.695356F,    0.352989F,     0.0400925F,
			  0.383482F,    0.188746F,    0.0193305F,    0.128885F,    -0.23603F,     -0.288163F,
			  -0.311799F,   -0.425027F,   -0.297739F,    -0.349681F,   -0.278894F,    0.00934887F,
			  -0.38221F,    0.542819F,    0.234533F,     -0.213422F,   0.198418F,     0.694582F,
			  -0.43395F,    -0.417672F,   0.553686F,     -0.10748F,    -0.352711F,    -0.0115025F,
			  0.0581546F,   0.962054F,    0.210576F,     0.339536F,    -0.0818458F,   -0.358587F,
			  -0.342001F,   -0.0689676F,  0.0470595F,    -0.3791F,     0.212149F,     -0.00608754F,
			  0.318279F,    0.246769F,    0.514428F,     0.457749F,    0.759536F,     0.236433F,
			  0.422228F,    0.571146F,    -0.247402F,    0.667306F,    -0.558038F,    -0.158556F,
			  -0.369374F,   -0.341798F,   0.30697F,      -0.535024F,   -0.487844F,    -0.0888073F,
			  0.404439F,    -0.580029F,   0.457389F,     0.297961F,    -0.0356712F,   0.508803F,
			  0.325652F,    -0.239089F,   -0.743984F,    0.21902F,     0.455838F,     0.149938F,
			  -0.150058F,   0.342239F,    0.147549F,     -0.044282F,   -0.634129F,    0.266822F,
			  -0.764306F,   -0.13691F,    -0.59542F,     -0.503302F,   -0.581097F,    0.455914F,
			  0.193022F,    -0.255091F,   0.0782733F,    0.354385F,    0.181455F,     -0.579845F,
			  -0.597151F,   -0.747541F,   -0.471478F,    -0.257622F,   0.80429F,      0.908564F,
			  0.11331F,     -0.210526F,   0.893246F,     -0.354708F,   -0.581153F,    0.366957F,
			  0.000682831F, 1.05443F,     0.310998F,     0.455284F,    -0.251732F,    -0.567471F,
			  -0.660306F,   -0.202108F,   0.836359F,     -0.467352F,   -0.20453F,     0.0710459F,
			  0.0628843F,   -0.132979F,   -0.755594F,    0.0600963F,   0.725805F,     -0.221625F,
			  0.133578F,    -0.802764F,   0.00850201F,   0.748137F,    -0.411616F,    -0.136451F,
			  0.0531707F,   -0.977616F,   0.162951F,     0.0394506F,   -0.0480862F,   0.797194F,
			  0.52012F,     0.238174F,    0.169073F,     0.249234F,    0.00133944F,   -0.01138F,
			  0.107195F,    0.0101681F,   -0.247766F,    -0.415877F,   -0.450288F,    0.800731F};
			jitterTableIndex += 2;
			if (jitterTableIndex == jitterTable.size())
				jitterTableIndex = 0;
			return Vector2{jitterTable[jitterTableIndex], jitterTable[jitterTableIndex + 1]};
		}
	} // namespace draw
} // namespace spades