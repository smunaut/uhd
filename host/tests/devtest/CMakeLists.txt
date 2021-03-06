# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
#

# Formatting
MESSAGE(STATUS "")

#
# Arguments:
# - pattern: This will be used to identify which devtest_*.py is to be executed.
# - filter: Will be used in args strings as "type=<filter>".
# - devtype: A descriptive string. Is only used for CMake output.
MACRO(ADD_DEVTEST pattern filter devtype)
    MESSAGE(STATUS "Adding ${devtype} device test target")
    ADD_CUSTOM_TARGET("test_${pattern}"
        ${PYTHON_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/run_testsuite.py
        "--src-dir" "@CMAKE_CURRENT_SOURCE_DIR@"
        "--devtest-pattern" "${pattern}"
        "--device-filter" "${filter}"
        "--build-type" "${CMAKE_BUILD_TYPE}"
        "--build-dir" "${CMAKE_BINARY_DIR}"
        COMMENT "Running device test on all connected ${devtype} devices:"
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    )
ENDMACRO(ADD_DEVTEST)

IF(ENABLE_B200)
    ADD_DEVTEST("b2xx" "b200" "B2XX")
ENDIF(ENABLE_B200)
IF(ENABLE_X300)
    ADD_DEVTEST("x3x0" "x300" "X3x0")
ENDIF(ENABLE_X300)

# Formatting
MESSAGE(STATUS "")
